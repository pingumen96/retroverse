#include <rv/esde/rom_scanner.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include <windows.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/metadata.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/core/utf8.hpp>

namespace rv::esde {
namespace {

constexpr std::string_view kNoLoadFile = "noload.txt";
constexpr std::string_view kNoLoadKey = "NOLOAD.TXT";

/// The last element of a relative path ('/' between the elements).
std::string_view nameOf(std::string_view relativePath) noexcept {
    const std::size_t slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1);
}

/// What the walk listed, before ES-DE's rules are applied: in walk order, each folder before its content.
struct RawItem {
    catalog::LookupKey key;
    catalog::FileFacts facts;
    /// The folder holding it, an index into the raw items; npos for an item of the system folder.
    std::size_t parent = std::string::npos;
    /// A folder whose content holds a noload.txt (any case).
    bool holdsNoLoad = false;
};

/// Whether the folder `folder` (absolute) is hidden as ES-DE's Utils::FileSystem::isHidden tells: the hidden attribute
/// or a name with a leading dot.
bool folderHidden(const std::filesystem::path& folder) {
    const DWORD attributes = GetFileAttributesW(core::extendedLengthPath(folder).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
        return true;
    }
    const std::string name = core::pathToUtf8Lossy(folder.filename());
    return !name.empty() && name.front() == '.';
}

catalog::SourceUnavailableReason unavailableReasonOf(ErrorCode code) noexcept {
    return code == ErrorCode::NotFound ? catalog::SourceUnavailableReason::FolderMissing
                                       : catalog::SourceUnavailableReason::NotListable;
}

} // namespace

std::string_view esdeExtension(std::string_view name) noexcept {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return ".";
    }
    return name.substr(dot); // "name." gives "."
}

std::string_view esdeStem(std::string_view name, bool directory) noexcept {
    if (directory || name == ".") {
        return name;
    }
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

std::string esdeDefaultName(std::string_view relativePath, EsdeNameKind kind, const ScanRules& rules,
                            std::string* esdeName) {
    const std::string_view name = nameOf(relativePath);
    const bool directory = kind != EsdeNameKind::File;
    std::string own;
    std::string shown;
    if (rules.arcade && kind != EsdeNameKind::Folder) {
        // FileData's constructor for an arcade or neogeo game: MameNames::getCleanName(FileData::getCleanName()), the
        // stem of a directory being its whole name.
        static const MameSets kNoLists;
        own = mameName(rules.mame != nullptr ? *rules.mame : kNoLists, esdeStem(name, directory),
                       rules.mameNameStripExtraInfo);
        shown = own;
        if (kind == EsdeNameKind::GameDirectory) {
            // populateFolder then cuts as many bytes as the extension has off a game directory's name.
            const std::size_t extension = esdeExtension(name).size();
            shown.resize(shown.size() >= extension ? shown.size() - extension : 0);
        }
        if (!core::isValidUtf8(shown)) {
            shown.clear();
        }
        shown = catalog::displayText(shown);
    } else {
        // getDisplayName(): the stem of a file, the whole name of a folder (a game directory's loses its extension).
        own = std::string(esdeStem(name, directory));
    }
    if (esdeName != nullptr) {
        *esdeName = std::move(own);
    }
    return shown.empty() ? catalog::titleFromFileStem(relativePath, kind == EsdeNameKind::Folder) : shown;
}

std::span<const ScannedItem> RomFolderListing::sameKey(std::string_view key) const noexcept {
    const auto lower = std::lower_bound(items.begin(), items.end(), key,
                                        [](const ScannedItem& item, std::string_view k) { return item.key.key() < k; });
    auto upper = lower;
    while (upper != items.end() && upper->key.key() == key) {
        ++upper;
    }
    return {lower, upper};
}

Expected<RomFolderListing> scanRomFolder(const std::filesystem::path& romFolder, const catalog::SystemName& system,
                                         const ScanRules& rules) {
    RomFolderListing listing;
    const std::string systemName = system.str();
    const auto isConfigured = [&](std::string_view extension, bool directory) {
        if (directory && extension == ".") {
            return false; // a directory without an extension is never a game, even where "." is configured
        }
        return std::find(rules.extensions.begin(), rules.extensions.end(), extension) != rules.extensions.end();
    };
    const auto isArcadeAsset = [&](std::string_view name, bool directory) {
        return rules.arcade && rules.mame != nullptr && rules.mame->isBiosOrDevice(esdeStem(name, directory));
    };
    // A directory ES-DE lists as a game rather than entering it: a configured extension, not a BIOS or device set, and
    // a name ES-DE does not leave empty (populateFolder falls through to the folder rules otherwise).
    const auto isGameDirectory = [&](std::string_view name) {
        if (!isConfigured(esdeExtension(name), true) || isArcadeAsset(name, true)) {
            return false;
        }
        static const MameSets kNoLists;
        return !rules.arcade ||
               !mameName(rules.mame != nullptr ? *rules.mame : kNoLists, name, rules.mameNameStripExtraInfo).empty();
    };
    const std::string folderText = core::pathToUtf8Lossy(romFolder);
    const auto issue = [&](catalog::IssueSeverity severity, std::string_view code, std::string_view relativePath,
                           std::string reason) {
        std::string file = folderText + "\\" + std::string(relativePath);
        std::replace(file.begin() + static_cast<std::ptrdiff_t>(folderText.size()), file.end(), '/', '\\');
        listing.issues.push_back(
            catalog::Issue{severity, std::string(code), systemName, std::move(file), std::move(reason)});
    };

    // The walk: every file and folder, in walk order, with its folder; nothing inside a would-be game directory.
    std::vector<RawItem> raw;
    std::vector<std::size_t> folders; // the folders the walk is inside, outermost first (indices into raw)
    std::string lastDirectory;        // the relative path of the last folder listed: an Error entry after it is its own
    bool lastWasDirectory = false;
    core::TreeWalkOptions walkOptions;
    walkOptions.followLinks = true;
    walkOptions.cancel = rules.cancel;
    auto walked = core::walkTree(
        romFolder,
        [&](const core::TreeEntry& entry) {
            if (entry.kind == core::TreeEntryKind::Error) {
                if (lastWasDirectory && entry.relativePath == lastDirectory) {
                    listing.unavailableFolders.push_back(catalog::UnavailableFolder{
                        system, entry.relativePath, unavailableReasonOf(entry.error.code), entry.error.message});
                    issue(catalog::IssueSeverity::Error, kRomIssueFolderUnavailable, entry.relativePath,
                          fmt::format("cannot be listed ({}): its content is unknown, kept as source unavailable",
                                      entry.error.message));
                } else {
                    issue(catalog::IssueSeverity::Warning, kRomIssueEntryUnreadable, entry.relativePath,
                          fmt::format("cannot be listed: {}", entry.error.message));
                }
                lastWasDirectory = false;
                return core::WalkAction::Continue;
            }
            const bool directory = entry.kind == core::TreeEntryKind::Directory;
            lastWasDirectory = directory;
            lastDirectory = directory ? entry.relativePath : std::string();
            while (!folders.empty() && !entry.relativePath.starts_with(raw[folders.back()].key.relativePath() + "/")) {
                folders.pop_back();
            }
            auto key = catalog::makeLookupKey(entry.relativePath);
            if (!key) {
                issue(catalog::IssueSeverity::Warning, kRomIssueNameUnsupported, entry.relativePath,
                      fmt::format("{}: left out", key.error().message));
                return directory ? core::WalkAction::SkipChildren : core::WalkAction::Continue;
            }
            RawItem item;
            item.key = std::move(*key);
            item.facts.directory = directory;
            item.facts.size = directory ? 0 : entry.size;
            item.facts.lastWriteTime = catalog::fileTimeTicks(entry.lastWriteTime);
            const std::string_view name = nameOf(entry.relativePath);
            item.facts.hidden = entry.hidden || name.starts_with('.');
            item.parent = folders.empty() ? std::string::npos : folders.back();
            if (item.parent != std::string::npos && nameOf(item.key.key()) == kNoLoadKey) {
                raw[item.parent].holdsNoLoad = true;
            }
            if (item.parent == std::string::npos && name == kNoLoadFile) {
                listing.noLoadAtRoot = true;
            }
            raw.push_back(std::move(item));
            if (!directory) {
                return core::WalkAction::Continue;
            }
            // ES-DE does not look into a directory that would be a game (whether or not something excludes it).
            if (isGameDirectory(name)) {
                return core::WalkAction::SkipChildren;
            }
            folders.push_back(raw.size() - 1);
            return core::WalkAction::Continue;
        },
        walkOptions);
    if (!walked) {
        return propagate(std::move(walked.error()));
    }
    if (walked->cancelled) {
        return makeError(ErrorCode::Cancelled, "the scan was cancelled", core::pathToUtf8Lossy(romFolder));
    }
    listing.filesListed = walked->files;
    listing.foldersListed = walked->directories;
    listing.systemFolderHidden = folderHidden(romFolder);

    // ES-DE's rules, in walk order: a folder's verdict is known before its content's.
    listing.items.reserve(raw.size());
    std::vector<ScanExclusion> blocksContent(raw.size(), ScanExclusion::None); // what a folder imposes on its content
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const RawItem& source = raw[i];
        const std::string_view name = nameOf(source.key.relativePath());
        const bool directory = source.facts.directory;
        ScannedItem item;
        item.key = source.key;
        item.facts = source.facts;
        item.extensionConfigured = isConfigured(esdeExtension(name), directory);
        item.arcadeAsset = isArcadeAsset(name, directory);
        item.parentHidden =
            source.parent == std::string::npos ? listing.systemFolderHidden : raw[source.parent].facts.hidden;
        item.above = source.parent == std::string::npos ? ScanExclusion::None : blocksContent[source.parent];
        const bool hiddenSkipped = !rules.showHiddenFiles && item.facts.hidden;

        std::string esdeName;
        const bool gameDirectory = directory && isGameDirectory(name);
        item.defaultName = esdeDefaultName(source.key.relativePath(),
                                           !directory      ? EsdeNameKind::File
                                           : gameDirectory ? EsdeNameKind::GameDirectory
                                                           : EsdeNameKind::Folder,
                                           rules, &esdeName);
        if (directory) {
            // A would-be game directory that is a BIOS or device set, or whose name ES-DE leaves empty, is entered as a
            // folder (populateFolder deletes the game and falls through to the folder rules).
            const bool game = gameDirectory;
            if (item.above != ScanExclusion::None) {
                item.verdict = game ? ScanVerdict::Excluded : ScanVerdict::Folder;
                item.exclusion = game ? item.above : ScanExclusion::None;
                blocksContent[i] = item.above;
            } else if (hiddenSkipped) {
                item.verdict = game ? ScanVerdict::Excluded : ScanVerdict::Folder;
                item.exclusion = ScanExclusion::Hidden;
                blocksContent[i] = ScanExclusion::Hidden;
            } else if (game) {
                item.verdict = ScanVerdict::Game;
            } else {
                item.verdict = ScanVerdict::Folder;
                if (source.holdsNoLoad) {
                    item.exclusion = ScanExclusion::NoLoadTxt;
                    blocksContent[i] = ScanExclusion::NoLoadTxt;
                }
            }
        } else if (!item.extensionConfigured) {
            item.verdict = ScanVerdict::NotConfigured;
        } else if (item.above != ScanExclusion::None) {
            item.verdict = ScanVerdict::Excluded;
            item.exclusion = item.above;
        } else if (hiddenSkipped) {
            item.verdict = ScanVerdict::Excluded;
            item.exclusion = ScanExclusion::Hidden;
        } else if (esdeName.empty()) {
            item.verdict = ScanVerdict::NameEmpty;
            issue(catalog::IssueSeverity::Warning, kRomIssueNameEmpty, source.key.relativePath(),
                  "its name, as ES-DE makes it from the file name, is empty: ES-DE skips it \"as it has no "
                  "filename\" unless its gamelist names it");
        } else if (item.arcadeAsset) {
            item.verdict = ScanVerdict::Excluded;
            item.exclusion = ScanExclusion::BiosOrDeviceSet;
        } else {
            item.verdict = ScanVerdict::Game;
        }
        listing.items.push_back(std::move(item));
    }
    std::sort(listing.items.begin(), listing.items.end(),
              [](const ScannedItem& a, const ScannedItem& b) { return a.key < b.key; });
    std::sort(listing.unavailableFolders.begin(), listing.unavailableFolders.end(),
              [](const catalog::UnavailableFolder& a, const catalog::UnavailableFolder& b) {
                  return a.relativePath < b.relativePath;
              });
    spdlog::debug("ROM folder of {}: {} file(s), {} folder(s), {} unavailable folder(s), {} issue(s)", systemName,
                  listing.filesListed, listing.foldersListed, listing.unavailableFolders.size(), listing.issues.size());
    return listing;
}

} // namespace rv::esde
