#include <rv/esde/media_resolver.hpp>

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

#include <windows.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/core/utf8.hpp>

namespace rv::esde {
namespace {

namespace fs = std::filesystem;
using catalog::IssueSeverity;
using catalog::MediaKind;

/// The most folders that could not be listed reported one by one per kind folder; the others are counted in one more
/// issue (a broken tree could hold thousands).
constexpr std::size_t kUnlistedFoldersListed = 100;

/// Text folded as the lookup keys are (core::fileSystemUpperCase()). ASCII text, nearly every media name, takes a short
/// way with the same result: the file systems' table maps 'a' to 'z' onto 'A' to 'Z' and keeps every other ASCII
/// character.
Expected<std::string> folded(std::string_view text) {
    const bool ascii =
        std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80u; });
    if (!ascii) {
        return core::fileSystemUpperCase(text);
    }
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

/// The extensions of a kind, folded, in ES-DE's order.
const std::array<std::vector<std::string>, catalog::kMediaKindCount>& foldedExtensions() {
    static const std::array<std::vector<std::string>, catalog::kMediaKindCount> table = [] {
        std::array<std::vector<std::string>, catalog::kMediaKindCount> result;
        for (const MediaKind kind : catalog::kMediaKinds) {
            for (const std::string_view extension : mediaExtensions(kind)) {
                result[static_cast<std::size_t>(kind)].push_back(folded(extension).value_or(std::string(extension)));
            }
        }
        return result;
    }();
    return table;
}

/// The text of the folder `relative` ('/' between the elements, as listed) below the media root, for an issue's file.
std::string folderText(const fs::path& root, std::string_view relative) {
    fs::path folder = root / core::pathFromUtf8(relative);
    return core::pathToUtf8Lossy(folder.make_preferred());
}

/// "<system>/<kind folder>": how messages name a kind folder (relative to the media root, as ES-DE names it).
std::string kindFolderText(const catalog::SystemName& system, MediaKind kind) {
    return fmt::format("{}/{}", system.str(), catalog::toString(kind));
}

/// Of `names`, the names as listed that fold to one folded name, the one ES-DE takes for `name`: the one spelled
/// exactly like it, else, in a folder without case-sensitive names, the first (the walk's ordinal order; there can be
/// only one there); nothing when there is none.
const std::string* pick(const std::vector<std::string>& names, std::string_view name, bool caseSensitive) {
    const auto exact = std::find(names.begin(), names.end(), name);
    if (exact != names.end()) {
        return &*exact;
    }
    return names.empty() || caseSensitive ? nullptr : &names.front();
}

/// FNV-1a, 64 bits: what detail::MediaFile keeps of a name's spelling.
std::uint64_t spellingHash(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Whether `folder` has case-sensitive names: NTFS's per-folder flag (FILE_CS_FLAG_CASE_SENSITIVE_DIR), which
/// `fsutil file setCaseSensitiveInfo` sets and WSL sets on the folders it makes on Windows drives (folders made inside
/// such a folder get it too). False on a file system without it (exFAT, FAT32, most shares); nothing when the folder
/// cannot be opened. Only the folder's attributes are read.
std::optional<bool> caseSensitiveFolder(const fs::path& folder) {
    const HANDLE handle =
        CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    FILE_CASE_SENSITIVE_INFO info{};
    const bool sensitive = GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &info, sizeof(info)) != FALSE &&
                           (info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0;
    CloseHandle(handle);
    return sensitive;
}

/// ES-DE's existence test of a media file (Utils::FileSystem::exists(): std::filesystem::exists()) on `file`, an
/// extended-length path: the file system's own lookup, which compares each name as the folder holding it does, a link
/// followed to its target (one whose target cannot be opened is not there). A folder is no media file (Retroverse's
/// rule, as in the listings). Errors: Unavailable when the lookup fails otherwise than for a missing name (a drive
/// gone, access denied).
Expected<bool> mediaFileExists(const fs::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        const DWORD error = GetLastError();
        if (core::fileErrorCode(error) == ErrorCode::NotFound) {
            return false;
        }
        return makeError(ErrorCode::Unavailable, core::systemErrorMessage(error), core::pathToUtf8Lossy(file));
    }
    DWORD attributes = data.dwFileAttributes;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        const HANDLE handle =
            CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        BY_HANDLE_FILE_INFORMATION information{};
        const bool read = GetFileInformationByHandle(handle, &information) != FALSE;
        CloseHandle(handle);
        if (!read) {
            return false;
        }
        attributes = information.dwFileAttributes;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/// A walk's root error as a folder state: NotFound (gone) and InvalidArgument (a file) are a missing folder,
/// everything else (Unavailable) a folder that cannot be listed.
MediaFolderState stateOf(const Error& error) noexcept {
    return error.code == ErrorCode::NotFound || error.code == ErrorCode::InvalidArgument
               ? MediaFolderState::Missing
               : MediaFolderState::Unavailable;
}

catalog::Issue issueOf(IssueSeverity severity, std::string_view code, const catalog::SystemName& system,
                       std::string file, std::string reason) {
    return catalog::Issue{severity, std::string(code), system.str(), std::move(file), std::move(reason)};
}

/// "a", "a and b", "a, b and c".
std::string listText(std::span<const std::string_view> items) {
    std::string text;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            text += i + 1 == items.size() ? " and " : ", ";
        }
        text += items[i];
    }
    return text;
}

class Stopwatch {
public:
    [[nodiscard]] double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    }

private:
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

} // namespace

std::span<const std::string_view> mediaExtensions(MediaKind kind) noexcept {
    switch (kind) {
    case MediaKind::Video:
        return kVideoExtensions;
    case MediaKind::Manual:
        return kManualExtensions;
    default:
        return kImageExtensions;
    }
}

std::string mediaRelativePath(const catalog::SystemName& system, const catalog::LookupKey& key, bool directory,
                              MediaKind kind, std::string_view extension) {
    return fmt::format("{}/{}/{}{}", system.str(), catalog::toString(kind),
                       catalog::mediaStem(key.relativePath(), directory), extension);
}

std::string_view toString(MediaFolderState state) noexcept {
    switch (state) {
    case MediaFolderState::Listed:
        return "listed";
    case MediaFolderState::Missing:
        return "missing";
    case MediaFolderState::Unavailable:
        return "unavailable";
    }
    return "unknown";
}

std::uint64_t MediaKindListing::ignoredFiles() const noexcept {
    std::uint64_t files = 0;
    for (const IgnoredExtension& extension : ignored) {
        files += extension.files;
    }
    return files;
}

std::uint64_t SystemMediaListing::media() const noexcept {
    std::uint64_t total = 0;
    for (const MediaKindListing& kind : kinds) {
        total += kind.media;
    }
    return total;
}

EsdeMediaResolver::EsdeMediaResolver(std::filesystem::path mediaRoot) : m_root(std::move(mediaRoot)) {}

EsdeMediaResolver::EsdeMediaResolver(const ResolvedRoot& mediaRoot) : m_root(mediaRoot.path) {
    if (!m_root) {
        m_unresolvedReason = "the media root could not be resolved";
        for (const catalog::Issue& issue : mediaRoot.issues) {
            if (issue.severity == IssueSeverity::Error) {
                m_unresolvedReason += ": " + issue.reason;
                break;
            }
        }
    }
}

Status EsdeMediaResolver::checkRoot() {
    if (m_rootStatus) {
        return *m_rootStatus;
    }
    if (!m_root) {
        m_rootStatus = makeError(ErrorCode::Unavailable, m_unresolvedReason, "media root");
        return *m_rootStatus;
    }
    // The root's folders, so that a system without a media folder costs no failed open and a folder spelled in
    // another case is found, as ES-DE finds it.
    const Stopwatch stopwatch;
    std::map<std::string, std::vector<std::string>> folders;
    const auto walked = core::walkTree(*m_root, [&](const core::TreeEntry& entry) {
        if (entry.kind == core::TreeEntryKind::Directory) {
            if (auto name = folded(entry.relativePath)) {
                folders[std::move(*name)].push_back(entry.relativePath);
            }
            return core::WalkAction::SkipChildren;
        }
        return core::WalkAction::Continue;
    });
    if (!walked) {
        m_stats.seconds += stopwatch.seconds();
        const std::string why = walked.error().code == ErrorCode::NotFound ? "the media root does not exist"
                                : walked.error().code == ErrorCode::InvalidArgument
                                    ? "the media root is not a folder"
                                    : "the media root cannot be read: " + walked.error().message;
        m_rootStatus = makeError(ErrorCode::Unavailable, why, core::pathToUtf8Lossy(*m_root));
        return *m_rootStatus;
    }
    ++m_stats.caseChecks;
    m_rootCaseSensitive = caseSensitiveFolder(fs::path(core::extendedLengthPath(*m_root))).value_or(false);
    m_stats.seconds += stopwatch.seconds();
    m_systemFolders = std::move(folders);
    m_rootStatus = Status{};
    return *m_rootStatus;
}

EsdeMediaResolver::SystemIndex EsdeMediaResolver::listSystem(const catalog::SystemName& system) {
    const Stopwatch stopwatch;
    SystemIndex index;
    SystemMediaListing& listing = index.listing;
    listing.system = system;
    ++m_stats.systemsListed;
    const auto finish = [&] {
        m_stats.seconds += stopwatch.seconds();
        spdlog::debug("Media of {}: {} ({} media files), {:.3f} s", system.str(), toString(listing.state),
                      listing.media(), stopwatch.seconds());
    };

    const auto systemFolded = folded(system.str());
    const auto found = systemFolded ? m_systemFolders.find(*systemFolded) : m_systemFolders.end();
    const std::string* folderName =
        found == m_systemFolders.end() ? nullptr : pick(found->second, system.str(), m_rootCaseSensitive);
    if (folderName == nullptr) {
        if (found != m_systemFolders.end()) {
            listing.issues.push_back(issueOf(
                IssueSeverity::Info, kMediaIssueCaseSensitiveFolder, system, core::pathToUtf8Lossy(*m_root),
                fmt::format("the media root has case-sensitive names: ES-DE does not take the folder \"{}\" for the "
                            "media folder of {}, which it asks for as \"{}\"",
                            found->second.front(), system.str(), system.str())));
        }
        finish(); // no media folder: no media
        return index;
    }
    listing.folderName = *folderName;
    const auto systemFolder = core::extendedLengthPathBelow(*m_root, listing.folderName);
    const std::string systemText = folderText(*m_root, listing.folderName);

    // The kind folders, found as ES-DE's file system finds them: case-insensitively.
    std::map<std::string, std::vector<std::string>> kindFolders;
    auto walked = systemFolder ? core::walkTree(*systemFolder,
                                                [&](const core::TreeEntry& entry) {
                                                    if (entry.kind == core::TreeEntryKind::Directory) {
                                                        if (auto name = folded(entry.relativePath)) {
                                                            kindFolders[std::move(*name)].push_back(
                                                                entry.relativePath);
                                                        }
                                                        return core::WalkAction::SkipChildren;
                                                    }
                                                    return core::WalkAction::Continue;
                                                })
                               : Expected<core::TreeWalkSummary>(tl::unexpected<Error>(systemFolder.error()));
    if (!walked) {
        listing.state = stateOf(walked.error());
        listing.folderName = listing.state == MediaFolderState::Missing ? std::string() : listing.folderName;
        if (listing.state == MediaFolderState::Unavailable) {
            listing.detail = walked.error().message;
            listing.issues.push_back(issueOf(
                IssueSeverity::Error, kMediaIssueFolderUnlisted, system, systemText,
                fmt::format("the media folder of {} cannot be listed ({}): whether its games have media is not known",
                            system.str(), listing.detail)));
        }
        finish();
        return index;
    }
    listing.state = MediaFolderState::Listed;
    ++m_stats.caseChecks;
    listing.caseSensitive = caseSensitiveFolder(*systemFolder).value_or(false);
    if (listing.caseSensitive) {
        listing.issues.push_back(
            issueOf(IssueSeverity::Info, kMediaIssueCaseSensitiveFolder, system, systemText,
                    fmt::format("the media folder of {} has case-sensitive names: ES-DE finds its kind folders only "
                                "spelled in lower case, as \"covers\" or \"videos\"",
                                system.str())));
    }

    const auto& extensions = foldedExtensions();
    for (const MediaKind kind : catalog::kMediaKinds) {
        const std::size_t k = static_cast<std::size_t>(kind);
        MediaKindListing& kindListing = listing.kinds[k];
        const auto kindFolded = folded(catalog::toString(kind));
        const auto candidates = kindFolded ? kindFolders.find(*kindFolded) : kindFolders.end();
        const std::string* kindName = candidates == kindFolders.end()
                                          ? nullptr
                                          : pick(candidates->second, catalog::toString(kind), listing.caseSensitive);
        if (kindName == nullptr) {
            if (candidates != kindFolders.end()) {
                listing.issues.push_back(issueOf(
                    IssueSeverity::Info, kMediaIssueCaseSensitiveFolder, system, systemText,
                    fmt::format("the media folder of {} has case-sensitive names: ES-DE does not take \"{}\" for its "
                                "{} folder, which it asks for as \"{}\"",
                                system.str(), candidates->second.front(), catalog::toString(kind),
                                catalog::toString(kind))));
            }
            continue; // Missing: no media of the kind
        }
        kindListing.folderName = *kindName;
        const std::string relativeFolder = listing.folderName + "/" + kindListing.folderName;
        const std::string kindText = folderText(*m_root, relativeFolder);
        const auto kindFolder = core::extendedLengthPathBelow(*m_root, relativeFolder);

        std::vector<detail::MediaFile>& files = index.files[k];
        std::vector<std::string>& unlistedFolded = index.unlistedFolded[k];
        std::map<std::string, std::uint64_t> ignored;
        std::string lastFolder;
        const auto visit = [&](const core::TreeEntry& entry) {
            switch (entry.kind) {
            case core::TreeEntryKind::Directory:
                ++m_stats.folders;
                lastFolder = entry.relativePath;
                return core::WalkAction::Continue;
            case core::TreeEntryKind::Error:
                if (!lastFolder.empty() && entry.relativePath == lastFolder) {
                    // The folder just reported could not be entered: what lies below it is not known.
                    kindListing.unlistedFolders.push_back(entry.relativePath);
                    if (auto name = folded(entry.relativePath)) {
                        unlistedFolded.push_back(std::move(*name));
                    }
                    if (kindListing.unlistedFolders.size() <= kUnlistedFoldersListed) {
                        listing.issues.push_back(issueOf(
                            IssueSeverity::Error, kMediaIssueFolderUnlisted, system, kindText,
                            fmt::format("the folder \"{}\" of {} cannot be listed ({}): whether the games below it "
                                        "have media is not known",
                                        entry.relativePath, kindFolderText(system, kind), entry.error.message)));
                    }
                } else {
                    ++kindListing.unreadableEntries;
                }
                return core::WalkAction::Continue;
            case core::TreeEntryKind::File:
                break;
            }
            ++m_stats.files;
            const std::string& path = entry.relativePath;
            const std::size_t slash = path.rfind('/');
            const std::size_t nameStart = slash == std::string::npos ? 0 : slash + 1;
            const std::size_t dot = path.rfind('.');
            if (dot == std::string::npos || dot < nameStart) {
                ++ignored[std::string()];
                return core::WalkAction::Continue;
            }
            const std::string_view extension = std::string_view(path).substr(dot);
            const auto extensionFolded = folded(extension);
            const auto& accepted = extensions[k];
            const auto rank = extensionFolded ? std::find(accepted.begin(), accepted.end(), *extensionFolded)
                                              : accepted.end();
            if (rank == accepted.end()) {
                ++ignored[std::string(extension)];
                return core::WalkAction::Continue;
            }
            auto name = folded(std::string_view(path).substr(0, dot));
            if (!name) {
                ++kindListing.unreadableEntries;
                return core::WalkAction::Continue;
            }
            const auto place = static_cast<std::uint8_t>(rank - accepted.begin());
            files.push_back(detail::MediaFile{std::move(*name), std::string(extension), place,
                                              extension == mediaExtensions(kind)[place],
                                              spellingHash(std::string_view(path).substr(0, dot))});
            return core::WalkAction::Continue;
        };
        auto walkedKind = kindFolder ? core::walkTree(*kindFolder, visit)
                                     : Expected<core::TreeWalkSummary>(tl::unexpected<Error>(kindFolder.error()));
        if (!walkedKind) {
            kindListing.state = stateOf(walkedKind.error());
            if (kindListing.state == MediaFolderState::Missing) {
                kindListing.folderName.clear();
                continue;
            }
            kindListing.detail = walkedKind.error().message;
            listing.issues.push_back(issueOf(
                IssueSeverity::Error, kMediaIssueFolderUnlisted, system, kindText,
                fmt::format("the media folder {} cannot be listed ({}): whether the games of {} have media is not "
                            "known",
                            kindFolderText(system, kind), kindListing.detail, system.str())));
            files.clear();
            unlistedFolded.clear();
            continue;
        }
        kindListing.state = MediaFolderState::Listed;
        ++m_stats.folders;
        ++m_stats.caseChecks;
        kindListing.caseSensitive = caseSensitiveFolder(*kindFolder).value_or(false);

        // One file per media name: the first of ES-DE's extensions, then the one spelled as ES-DE spells it, then the
        // first listed (stable: the walk's ordinal order).
        std::stable_sort(files.begin(), files.end(), [](const detail::MediaFile& a, const detail::MediaFile& b) {
            return std::tie(a.name, a.rank, b.exactExtension) < std::tie(b.name, b.rank, a.exactExtension);
        });
        const auto last = std::unique(files.begin(), files.end(), [](const detail::MediaFile& a,
                                                                     const detail::MediaFile& b) {
            return a.name == b.name;
        });
        kindListing.shadowedFiles = static_cast<std::uint64_t>(files.end() - last);
        files.erase(last, files.end());
        files.shrink_to_fit();
        kindListing.media = files.size();
        std::sort(unlistedFolded.begin(), unlistedFolded.end());
        for (auto& [extension, count] : ignored) {
            kindListing.ignored.push_back(IgnoredExtension{extension, count});
        }

        // What ES-DE never shows of this folder, or shows only under its own spelling.
        if (kindListing.caseSensitive) {
            listing.issues.push_back(
                issueOf(IssueSeverity::Info, kMediaIssueCaseSensitiveFolder, system, kindText,
                        fmt::format("the media folder {} has case-sensitive names: ES-DE finds a media file in it only "
                                    "spelled as the game's file, with the extension in lower case",
                                    kindFolderText(system, kind))));
        }
        if (kindListing.unlistedFolders.size() > kUnlistedFoldersListed) {
            listing.issues.push_back(issueOf(
                IssueSeverity::Error, kMediaIssueFolderUnlisted, system, kindText,
                fmt::format("{} more folder(s) of {} cannot be listed, not listed one by one after the first {}",
                            kindListing.unlistedFolders.size() - kUnlistedFoldersListed, kindFolderText(system, kind),
                            kUnlistedFoldersListed)));
        }
        for (const IgnoredExtension& extension : kindListing.ignored) {
            listing.issues.push_back(issueOf(
                IssueSeverity::Info, kMediaIssueIgnoredExtension, system, kindText,
                fmt::format("{} file(s) {} in {}: ES-DE reads only {} there", extension.files,
                            extension.extension.empty() ? std::string("without an extension")
                                                        : fmt::format("with the extension \"{}\"", extension.extension),
                            kindFolderText(system, kind), listText(mediaExtensions(kind)))));
        }
        if (kindListing.shadowedFiles > 0) {
            listing.issues.push_back(issueOf(
                IssueSeverity::Info, kMediaIssueShadowedFile, system, kindText,
                fmt::format("{} file(s) in {} never used: a file of the same name has an extension ES-DE tries first "
                            "({}, in this order)",
                            kindListing.shadowedFiles, kindFolderText(system, kind),
                            listText(mediaExtensions(kind)))));
        }
        if (kindListing.unreadableEntries > 0) {
            listing.issues.push_back(issueOf(
                IssueSeverity::Warning, kMediaIssueUnreadableEntry, system, kindText,
                fmt::format("{} entr{} of {} could not be read (a link whose target cannot be opened, a name without "
                            "a UTF-8 form): no media",
                            kindListing.unreadableEntries, kindListing.unreadableEntries == 1 ? "y" : "ies",
                            kindFolderText(system, kind))));
        }
    }
    finish();
    return index;
}

Expected<EsdeMediaResolver::SystemIndex*> EsdeMediaResolver::index(const catalog::SystemName& system) {
    if (const Status root = checkRoot(); !root) {
        return propagate(root.error());
    }
    auto found = m_systems.find(system);
    if (found == m_systems.end()) {
        found = m_systems.emplace(system, listSystem(system)).first;
    }
    return &found->second;
}

bool EsdeMediaResolver::caseSensitiveOnTheWay(SystemIndex& index, MediaKind kind, std::string_view stem) {
    const std::size_t k = static_cast<std::size_t>(kind);
    const MediaKindListing& kindListing = index.listing.kinds[k];
    if (kindListing.caseSensitive) {
        return true;
    }
    // Each folder on the way, as the entry spells it: reached through folders without the flag, the file system finds
    // it under that spelling, and its folded path names it.
    for (std::size_t slash = stem.find('/'); slash != std::string_view::npos; slash = stem.find('/', slash + 1)) {
        const std::string_view folder = stem.substr(0, slash);
        const auto folderFolded = folded(folder);
        if (!folderFolded) {
            return true;
        }
        auto [known, inserted] = index.folderCase[k].try_emplace(*folderFolded, true);
        if (inserted) {
            ++m_stats.caseChecks;
            const auto path = core::extendedLengthPathBelow(
                *m_root, fmt::format("{}/{}/{}", index.listing.folderName, kindListing.folderName, folder));
            known->second = !path || caseSensitiveFolder(*path).value_or(true);
        }
        if (known->second) {
            return true;
        }
    }
    return false;
}

Expected<const SystemMediaListing*> EsdeMediaResolver::listing(const catalog::SystemName& system) {
    const auto found = index(system);
    if (!found) {
        return propagate(found.error());
    }
    return &(*found)->listing;
}

Expected<catalog::EntryMedia> EsdeMediaResolver::presence(const catalog::SystemName& system,
                                                          const catalog::LookupKey& key, bool directory) {
    if (key.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an entry outside its system folder has no media", system.str());
    }
    const auto found = index(system);
    if (!found) {
        return propagate(found.error());
    }
    SystemIndex& systemIndex = **found;
    const SystemMediaListing& listing = systemIndex.listing;
    catalog::EntryMedia media;
    if (listing.state == MediaFolderState::Unavailable) {
        return makeError(ErrorCode::Unavailable,
                         fmt::format("the media folder of {} cannot be listed: {}", system.str(), listing.detail),
                         key.relativePath());
    }
    if (listing.state == MediaFolderState::Missing) {
        return media;
    }
    const std::string name = catalog::mediaStem(key.key(), directory);
    const std::string spelled = catalog::mediaStem(key.relativePath(), directory);
    const std::uint64_t spelling = spellingHash(spelled);
    for (const MediaKind kind : catalog::kMediaKinds) {
        const std::size_t k = static_cast<std::size_t>(kind);
        const MediaKindListing& kindListing = listing.kinds[k];
        if (kindListing.state == MediaFolderState::Missing) {
            continue;
        }
        if (kindListing.state == MediaFolderState::Unavailable) {
            return makeError(ErrorCode::Unavailable,
                             fmt::format("the media folder {} cannot be listed: {}", kindFolderText(system, kind),
                                         kindListing.detail),
                             key.relativePath());
        }
        for (const std::string& folder : systemIndex.unlistedFolded[k]) {
            if (name.size() > folder.size() && name.starts_with(folder) && name[folder.size()] == '/') {
                return makeError(ErrorCode::Unavailable,
                                 fmt::format("a folder of {} that would hold the entry's media cannot be listed",
                                             kindFolderText(system, kind)),
                                 key.relativePath());
            }
        }
        const std::vector<detail::MediaFile>& files = systemIndex.files[k];
        const auto file = std::lower_bound(files.begin(), files.end(), name,
                                           [](const detail::MediaFile& a, const std::string& b) { return a.name < b; });
        if (file == files.end() || file->name != name) {
            continue;
        }
        // Spelled as ES-DE asks for it, or matched through the folding where every folder of the way folds names:
        // the listing's answer is ES-DE's.
        if ((file->spelling == spelling && file->exactExtension) ||
            !caseSensitiveOnTheWay(systemIndex, kind, spelled)) {
            media[kind] = catalog::MediaPresence{true, file->extension};
            continue;
        }
        // A folder of the way has case-sensitive names: ES-DE's own lookups, in its order.
        for (const std::string_view extension : mediaExtensions(kind)) {
            const auto candidate =
                core::extendedLengthPathBelow(*m_root, mediaRelativePath(system, key, directory, kind, extension));
            if (!candidate) {
                break; // a media name the file system cannot hold (an element "." or ".."): ES-DE finds nothing
            }
            ++m_stats.fileChecks;
            const auto exists = mediaFileExists(*candidate);
            if (!exists) {
                return makeError(ErrorCode::Unavailable,
                                 fmt::format("the media file of {} cannot be looked up: {}",
                                             kindFolderText(system, kind), exists.error().message),
                                 key.relativePath());
            }
            if (*exists) {
                media[kind] = catalog::MediaPresence{true, std::string(extension)};
                break;
            }
        }
    }
    return media;
}

Expected<std::filesystem::path> EsdeMediaResolver::path(const catalog::SystemName& system,
                                                        const catalog::LookupKey& key, bool directory, MediaKind kind,
                                                        const catalog::MediaPresence& media) const {
    if (key.empty()) {
        return makeError(ErrorCode::InvalidArgument, "an entry outside its system folder has no media", system.str());
    }
    if (!media.present) {
        return makeError(ErrorCode::NotFound,
                         fmt::format("the entry has no media of the kind {}", catalog::toString(kind)),
                         key.relativePath());
    }
    const std::string& extension = media.extension;
    const bool wellFormed =
        extension.size() >= 2 && extension.front() == '.' && core::isValidUtf8(extension) &&
        std::none_of(extension.begin(), extension.end(), [](char c) {
            return static_cast<unsigned char>(c) < 0x20u || c == '/' || c == '\\' || c == ':';
        });
    if (!wellFormed) {
        return makeError(ErrorCode::InvalidArgument, "the media's extension is no file extension", extension);
    }
    if (!m_root) {
        return makeError(ErrorCode::Unavailable, m_unresolvedReason, "media root");
    }
    fs::path file = *m_root / core::pathFromUtf8(mediaRelativePath(system, key, directory, kind, extension));
    file.make_preferred();
    return file;
}

void EsdeMediaResolver::refresh() noexcept {
    m_rootStatus.reset();
    m_rootCaseSensitive = false;
    m_systemFolders.clear();
    m_systems.clear();
    m_stats = MediaListingStats{};
}

} // namespace rv::esde
