#include <rv/esde/library_source.hpp>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

#include <windows.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/metadata.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/disc_groups.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/gamelist.hpp>
#include <rv/esde/mame_sets.hpp>
#include <rv/esde/rom_scanner.hpp>
#include <rv/esde/settings.hpp>
#include <rv/esde/systems.hpp>

namespace rv::esde {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using catalog::CatalogEntry;
using catalog::EntryState;
using catalog::NotAGameReason;

/// How many sheets are read at once (Loader::addDiscGroups).
constexpr std::size_t kSheetReaders = 8;

constexpr std::string_view kShowHiddenFiles = "ShowHiddenFiles";
constexpr std::string_view kShowHiddenGames = "ShowHiddenGames";
constexpr std::string_view kParseGamelistOnly = "ParseGamelistOnly";

/// The issues of one system: up to kSourceIssuesListedPerCode of each code, the others counted into one more issue of
/// the code when the system is done.
class IssueList {
public:
    void add(catalog::Issue issue) {
        Count& count = countOf(issue.code);
        if (++count.seen <= kSourceIssuesListedPerCode) {
            m_issues.push_back(std::move(issue));
        } else {
            count.last = std::move(issue);
        }
    }
    void addAll(std::vector<catalog::Issue> issues) {
        for (catalog::Issue& issue : issues) {
            add(std::move(issue));
        }
    }
    /// The issues listed, then the counts of those that were not, in the order their codes were first met.
    [[nodiscard]] std::vector<catalog::Issue> take() {
        for (const Count& count : m_counts) {
            if (count.seen > kSourceIssuesListedPerCode) {
                catalog::Issue summary = count.last;
                summary.reason =
                    fmt::format("{} more issue(s) of this kind, not listed one by one after the first {}; "
                                "the last: {}",
                                count.seen - kSourceIssuesListedPerCode, kSourceIssuesListedPerCode, count.last.reason);
                m_issues.push_back(std::move(summary));
            }
        }
        m_counts.clear();
        return std::move(m_issues);
    }

private:
    struct Count {
        std::string code;
        std::size_t seen = 0;
        catalog::Issue last;
    };
    Count& countOf(const std::string& code) {
        for (Count& count : m_counts) {
            if (count.code == code) {
                return count;
            }
        }
        m_counts.push_back(Count{code, 0, {}});
        return m_counts.back();
    }

    std::vector<catalog::Issue> m_issues;
    std::vector<Count> m_counts;
};

/// The last element of a relative path, and the one before it (empty for an item of the system folder).
std::string_view nameOf(std::string_view relativePath) noexcept {
    const std::size_t slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1);
}
std::string_view parentNameOf(std::string_view relativePath) noexcept {
    const std::size_t slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? std::string_view() : nameOf(relativePath.substr(0, slash));
}

/// A path below a folder as the issues name it: the folder, '\', the relative path with '\' between the elements.
std::string fileText(const fs::path& folder, std::string_view relativePath) {
    std::string text = core::pathToUtf8Lossy(folder);
    if (relativePath.empty()) {
        return text;
    }
    const std::size_t start = text.size() + 1;
    text += '\\';
    text += relativePath;
    std::replace(text.begin() + static_cast<std::ptrdiff_t>(start), text.end(), '/', '\\');
    return text;
}

void exclude(CatalogEntry& entry, NotAGameReason reason, std::string_view setting = {}) {
    entry.state = EntryState::NotAGame;
    entry.notAGameReason = reason;
    entry.excludingSetting = std::string(setting);
}

/// A gamelist entry's side of a catalog entry.
CatalogEntry entryFrom(const catalog::SystemName& system, const GamelistEntry& source) {
    CatalogEntry entry;
    entry.system = system;
    entry.key = source.key;
    entry.gamelistPath = source.path;
    entry.metadata = source.metadata;
    entry.raw = source.raw;
    entry.flags = source.flags;
    entry.altEmulator = source.altEmulator;
    return entry;
}

/// The title of an entry: its <name> as shown on one line, else `fallback`.
std::string titleOf(const catalog::GameMetadata& metadata, std::string fallback) {
    std::string title = catalog::displayText(metadata.name);
    return title.empty() ? std::move(fallback) : title;
}

/// The keys of the folders of a listing that could not be listed, with why.
using UnavailableKeys = std::map<std::string, catalog::SourceUnavailableReason, std::less<>>;

UnavailableKeys unavailableKeysOf(const RomFolderListing& listing) {
    UnavailableKeys keys;
    for (const catalog::UnavailableFolder& folder : listing.unavailableFolders) {
        if (auto key = catalog::makeLookupKey(folder.relativePath)) {
            keys.emplace(key->key(), folder.reason);
        }
    }
    return keys;
}

/// Why nothing is known of `key` (a lookup key): a folder above it could not be listed; nothing when every folder
/// above it was listed.
std::optional<catalog::SourceUnavailableReason> unavailableAbove(const UnavailableKeys& folders, std::string_view key) {
    for (std::size_t slash = key.find('/'); slash != std::string_view::npos && !folders.empty();
         slash = key.find('/', slash + 1)) {
        if (const auto folder = folders.find(key.substr(0, slash)); folder != folders.end()) {
            return folder->second;
        }
    }
    return std::nullopt;
}

/// Why ES-DE does not load a whole system.
enum class SystemBlock : std::uint8_t {
    None,
    Hidden, ///< its folder is hidden and ShowHiddenFiles is off
    NoLoad, ///< its folder holds a noload.txt (ParseGamelistOnly off)
};

/// A system's gamelist as read for the snapshot.
struct GamelistRead {
    catalog::GamelistState state = catalog::GamelistState::Absent;
    std::optional<Gamelist> gamelist;
};

/// One definition of a system read: what the snapshot takes of it when ES-DE would load it.
struct SystemResult {
    catalog::SystemInfo info;
    std::vector<CatalogEntry> entries;
    std::vector<catalog::DiscGroup> groups;
    std::vector<catalog::UnavailableFolder> unavailableFolders;
    std::vector<catalog::Issue> issues;
    /// The keys of the folders whose <folder> entry ES-DE applies with <hidden> while ShowHiddenGames is off: it deletes
    /// them with everything below (excludeBelowHiddenFolders()).
    std::vector<std::string> hiddenFolders;
    /// The games ES-DE would list (disc members included): whether ES-DE loads the definition.
    std::size_t esdeGames = 0;
};

/// With ShowHiddenGames off, ES-DE deletes a folder whose <folder> entry is <hidden>, and with it every game and folder
/// below it (GamelistFileParser::parseGamelist; ~FileData deletes the children): what is below such a folder is not a
/// game either. Missing files stay missing (ES-DE skips their entries before, as files that do not exist).
void excludeBelowHiddenFolders(SystemResult& result) {
    if (result.hiddenFolders.empty()) {
        return;
    }
    const std::set<std::string, std::less<>> folders(result.hiddenFolders.begin(), result.hiddenFolders.end());
    for (CatalogEntry& entry : result.entries) {
        if (entry.state != EntryState::Game && entry.state != EntryState::Folder) {
            continue;
        }
        const std::string_view key = entry.key.key();
        for (std::size_t slash = key.find('/'); slash != std::string_view::npos; slash = key.find('/', slash + 1)) {
            if (folders.contains(key.substr(0, slash))) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenGames);
                break;
            }
        }
    }
}

class Loader {
public:
    Loader(const catalog::LibraryProfile& profile, const catalog::LibraryLoadOptions& options,
           const EsdeLibrarySourceOptions& sourceOptions, EsdeLoadStatistics& statistics)
        : m_profile(profile), m_options(options), m_sourceOptions(sourceOptions), m_statistics(statistics) {}

    Expected<catalog::LibrarySnapshot> run();

private:
    [[nodiscard]] bool cancelled() const noexcept {
        return m_options.cancel != nullptr && m_options.cancel->load(std::memory_order_relaxed);
    }
    [[nodiscard]] tl::unexpected<Error> cancelledError() const {
        return makeError(ErrorCode::Cancelled, "the library load was cancelled",
                         core::pathToUtf8Lossy(m_profile.esdeHome));
    }
    const MameSets& mameSets();
    ScanRules rulesOf(const SystemDefinition& definition);
    Expected<SystemResult> loadSystem(const SystemDefinition& definition);
    Expected<GamelistRead> readSystemGamelist(const catalog::SystemName& system, const fs::path& romFolder,
                                              IssueList& issues);
    /// The entries of a system whose folder (or a folder below it) cannot be listed: what its gamelist alone tells.
    void addUnavailableEntries(SystemResult& result, const std::vector<const GamelistEntry*>& sameKey,
                               const ScanRules& rules, catalog::SourceUnavailableReason reason);
    void mergeListing(SystemResult& result, const RomFolderListing& listing, const Gamelist* gamelist,
                      const ScanRules& rules, SystemBlock block, const fs::path& gamelistFile, IssueList& issues);
    Status addDiscGroups(SystemResult& result, const RomFolderListing* listing, IssueList& issues);

    const catalog::LibraryProfile& m_profile;
    const catalog::LibraryLoadOptions& m_options;
    const EsdeLibrarySourceOptions& m_sourceOptions;
    EsdeLoadStatistics& m_statistics;
    EsdeSettings m_settings;
    ResolvedRoot m_romRoot;
    std::optional<MameLists> m_mame;
    std::vector<catalog::Issue> m_issues; ///< the snapshot's, in the order met
};

const MameSets& Loader::mameSets() {
    if (!m_mame) {
        m_mame = loadMameLists(m_profile);
        m_issues.insert(m_issues.end(), m_mame->issues.begin(), m_mame->issues.end());
    }
    return m_mame->sets;
}

ScanRules Loader::rulesOf(const SystemDefinition& definition) {
    ScanRules rules;
    rules.extensions = definition.extensions;
    rules.showHiddenFiles = m_settings.showHiddenFiles;
    rules.arcade = isArcadePlatform(definition.platforms);
    rules.mame = rules.arcade ? &mameSets() : nullptr;
    rules.mameNameStripExtraInfo = m_settings.mameNameStripExtraInfo;
    rules.cancel = m_options.cancel;
    return rules;
}

Expected<GamelistRead> Loader::readSystemGamelist(const catalog::SystemName& system, const fs::path& romFolder,
                                                  IssueList& issues) {
    const Clock::time_point start = Clock::now();
    const fs::path file = gamelistPath(m_profile.esdeHome, system);
    const std::string fileName = core::pathToUtf8Lossy(file);
    GamelistRead read;
    const auto failed = [&](std::string reason) {
        read.state = catalog::GamelistState::Failed;
        issues.add(catalog::Issue{catalog::IssueSeverity::Error, std::string(kSourceIssueGamelistFailed), system.str(),
                                  fileName, std::move(reason)});
    };
    if (romFolder.empty()) {
        // Without a ROM folder the gamelist's paths cannot be resolved: its metadata is not taken, nor taken as gone.
        if (GetFileAttributesW(core::extendedLengthPath(file).c_str()) != INVALID_FILE_ATTRIBUTES) {
            failed("the system's ROM folder is not known, so the gamelist's paths cannot be resolved: its metadata is "
                   "left as it was");
        }
        m_statistics.gamelistTime += Clock::now() - start;
        return read;
    }
    auto gamelist = readGamelist(file, system, romFolder);
    if (!gamelist && gamelist.error().code != ErrorCode::NotFound) {
        // ES-DE may be writing it (it rewrites a gamelist after every game played from it): read it again later.
        ++m_statistics.gamelistRetries;
        const Clock::time_point until = Clock::now() + m_sourceOptions.gamelistRetryDelay;
        while (Clock::now() < until) {
            if (cancelled()) {
                return cancelledError();
            }
            std::this_thread::sleep_for(std::min<Clock::duration>(until - Clock::now(), std::chrono::milliseconds(50)));
        }
        const Error first = gamelist.error();
        gamelist = readGamelist(file, system, romFolder);
        if (!gamelist && gamelist.error().code != ErrorCode::NotFound) {
            failed(fmt::format("cannot be read ({}), also after {} ms (first: {}): the system's entries have no "
                               "metadata, and the import keeps its previous rows",
                               gamelist.error().message, m_sourceOptions.gamelistRetryDelay.count(), first.message));
        }
    }
    if (gamelist) {
        read.state = catalog::GamelistState::Read;
        ++m_statistics.gamelistsRead;
        issues.addAll(gamelist->issues);
        gamelist->issues.clear();
        read.gamelist = std::move(*gamelist);
    }
    m_statistics.gamelistTime += Clock::now() - start;
    return read;
}

void Loader::addUnavailableEntries(SystemResult& result, const std::vector<const GamelistEntry*>& sameKey,
                                   const ScanRules& rules, catalog::SourceUnavailableReason reason) {
    // One entry per key: the last <game> entry, else the last <folder> one (which applies is the disk's to say).
    const GamelistEntry* chosen = nullptr;
    for (const GamelistEntry* entry : sameKey) {
        if (entry->kind == GamelistEntryKind::Game || chosen == nullptr || chosen->kind == GamelistEntryKind::Folder) {
            chosen = entry;
        }
    }
    if (chosen == nullptr) {
        return;
    }
    CatalogEntry entry = entryFrom(result.info.name, *chosen);
    entry.availability = catalog::Availability::SourceUnavailable;
    entry.unavailableReason = reason;
    const std::string& path = chosen->key.relativePath();
    const std::string_view name = nameOf(path);
    const bool dotted = name.starts_with('.') || parentNameOf(path).starts_with('.');
    if (chosen->kind == GamelistEntryKind::Folder) {
        entry.state = EntryState::Folder;
        entry.title = titleOf(entry.metadata, esdeDefaultName(path, EsdeNameKind::Folder, rules));
        if (!m_settings.showHiddenFiles && dotted) {
            exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
        } else if (!m_settings.showHiddenGames && entry.flags.hidden) {
            exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenGames);
            result.hiddenFolders.push_back(chosen->key.key());
        }
    } else {
        entry.state = EntryState::Game;
        entry.title = titleOf(entry.metadata, esdeDefaultName(path, EsdeNameKind::File, rules));
        const std::string_view extension = esdeExtension(name);
        if (!m_settings.showHiddenFiles && dotted) {
            exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
        } else if (std::find(rules.extensions.begin(), rules.extensions.end(), extension) == rules.extensions.end()) {
            exclude(entry, NotAGameReason::ExtensionNotConfigured);
        } else if (rules.arcade && rules.mame != nullptr && rules.mame->isBiosOrDevice(esdeStem(name, false))) {
            exclude(entry, NotAGameReason::BiosOrDeviceSet);
        } else if (!m_settings.showHiddenGames && entry.flags.hidden) {
            exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenGames);
        }
    }
    result.entries.push_back(std::move(entry));
}

void Loader::mergeListing(SystemResult& result, const RomFolderListing& listing, const Gamelist* gamelist,
                          const ScanRules& rules, SystemBlock block, const fs::path& gamelistFile, IssueList& issues) {
    const catalog::SystemName& system = result.info.name;
    const bool parseGamelistOnly = m_settings.parseGamelistOnly;
    const std::string gamelistText = core::pathToUtf8Lossy(gamelistFile);
    const auto entryIssue = [&](std::string_view code, const GamelistEntry& entry, std::string what) {
        issues.add(catalog::Issue{
            catalog::IssueSeverity::Warning, std::string(code), system.str(), gamelistText,
            fmt::format("line {}: the <{}> entry \"{}\" {}", entry.line, toString(entry.kind), entry.path, what)});
    };

    // The gamelist's entries by key, each key's in document order (lookups only).
    const std::vector<GamelistEntry> noEntries;
    const std::vector<GamelistEntry>& entries = gamelist != nullptr ? gamelist->entries : noEntries;
    std::unordered_map<std::string, std::vector<const GamelistEntry*>> byKey;
    for (const GamelistEntry& entry : entries) {
        if (!entry.outsideSystemFolder()) {
            byKey[entry.key.key()].push_back(&entry);
        }
    }
    std::unordered_map<const GamelistEntry*, bool> used;

    // What ES-DE makes of every file and folder listed.
    for (const ScannedItem& item : listing.items) {
        const GamelistEntryKind kind = item.enteredFolder() ? GamelistEntryKind::Folder : GamelistEntryKind::Game;
        const GamelistEntry* chosen = nullptr;
        if (const auto found = byKey.find(item.key.key()); found != byKey.end()) {
            const GamelistEntry* applies = entryForListed(found->second, item.key, kind);
            for (const GamelistEntry* entry : found->second) {
                if (!used[entry]) {
                    used[entry] = true;
                    if (entry->kind != kind) {
                        entryIssue(kSourceIssueKindMismatch, *entry,
                                   kind == GamelistEntryKind::Folder
                                       ? "names a folder ES-DE enters: skipped, as ES-DE skips a Game/folder mismatch"
                                       : "names a file or a game directory: skipped, as ES-DE skips a Game/folder "
                                         "mismatch");
                    }
                }
            }
            chosen = applies != nullptr && applies->kind == kind ? applies : nullptr;
        }
        CatalogEntry entry;
        if (chosen != nullptr) {
            entry = entryFrom(system, *chosen);
            entry.key = item.key; // the path as listed on disk, also when the gamelist writes it in another case
            entry.title = titleOf(entry.metadata, item.defaultName);
        } else if (kind == GamelistEntryKind::Folder || item.verdict == ScanVerdict::NotConfigured ||
                   item.verdict == ScanVerdict::NameEmpty) {
            continue; // no entry: a folder without a <folder> entry, a file ES-DE never lists
        } else {
            entry.system = system;
            entry.key = item.key;
            entry.title = item.defaultName;
        }
        entry.file = item.facts;
        const bool hiddenSkipped = !m_settings.showHiddenFiles && (item.facts.hidden || item.parentHidden);
        if (kind == GamelistEntryKind::Folder) {
            entry.state = EntryState::Folder;
            if (block == SystemBlock::Hidden) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
            } else if (block == SystemBlock::NoLoad) {
                exclude(entry, NotAGameReason::NoLoadTxt);
            } else if (hiddenSkipped || item.above == ScanExclusion::Hidden) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
            } else if (!parseGamelistOnly &&
                       (item.above == ScanExclusion::NoLoadTxt || item.exclusion == ScanExclusion::NoLoadTxt)) {
                exclude(entry, NotAGameReason::NoLoadTxt);
            } else if (!m_settings.showHiddenGames && entry.flags.hidden) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenGames);
            }
            // Whether ES-DE applies the entry (spelled as the folder is: its tree holds the names on disk), and so
            // deletes the folder with what is below it: not skipped for a hidden file or parent, not at a noload
            // folder; a hidden folder further up does not keep it from applying, as game entries below create the
            // folders between.
            if (!m_settings.showHiddenGames && chosen->flags.hidden && block == SystemBlock::None && !hiddenSkipped &&
                (parseGamelistOnly ||
                 (item.above != ScanExclusion::NoLoadTxt && item.exclusion != ScanExclusion::NoLoadTxt)) &&
                chosen->key.relativePath() == item.key.relativePath()) {
                result.hiddenFolders.push_back(item.key.key());
            }
        } else if (chosen != nullptr) {
            // GamelistFileParser::parseGamelist's checks, in its order.
            entry.state = EntryState::Game;
            if (block == SystemBlock::Hidden) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
            } else if (block == SystemBlock::NoLoad) {
                exclude(entry, NotAGameReason::NoLoadTxt);
            } else if (hiddenSkipped) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
            } else if (!parseGamelistOnly && item.above == ScanExclusion::NoLoadTxt) {
                exclude(entry, NotAGameReason::NoLoadTxt);
            } else if (!item.extensionConfigured) {
                exclude(entry, NotAGameReason::ExtensionNotConfigured);
            } else if (item.arcadeAsset) {
                exclude(entry, NotAGameReason::BiosOrDeviceSet);
            } else if (!m_settings.showHiddenGames && entry.flags.hidden) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenGames);
            }
        } else {
            // SystemData::populateFolder's verdict, unless ES-DE does not populate the system at all.
            entry.state = EntryState::Game;
            if (block == SystemBlock::Hidden) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
            } else if (parseGamelistOnly) {
                exclude(entry, NotAGameReason::ExcludedBySetting, kParseGamelistOnly);
            } else if (block == SystemBlock::NoLoad) {
                exclude(entry, NotAGameReason::NoLoadTxt);
            } else if (item.verdict == ScanVerdict::Excluded) {
                switch (item.exclusion) {
                case ScanExclusion::Hidden:
                    exclude(entry, NotAGameReason::ExcludedBySetting, kShowHiddenFiles);
                    break;
                case ScanExclusion::NoLoadTxt:
                    exclude(entry, NotAGameReason::NoLoadTxt);
                    break;
                case ScanExclusion::BiosOrDeviceSet:
                    exclude(entry, NotAGameReason::BiosOrDeviceSet);
                    break;
                case ScanExclusion::None:
                    break;
                }
            }
        }
        result.entries.push_back(std::move(entry));
    }

    // The entries that name nothing listed, a key at a time, in document order.
    const UnavailableKeys unavailableFolders = unavailableKeysOf(listing);
    for (const GamelistEntry& source : entries) {
        if (used[&source]) {
            continue;
        }
        if (source.outsideSystemFolder()) {
            used[&source] = true;
            CatalogEntry entry = entryFrom(system, source);
            entry.state = EntryState::OutsideSystemFolder;
            entry.title = titleOf(entry.metadata, catalog::titleFromFileStem(source.path));
            result.entries.push_back(std::move(entry));
            continue;
        }
        const std::vector<const GamelistEntry*>& sameKey = byKey[source.key.key()];
        for (const GamelistEntry* entry : sameKey) {
            used[entry] = true;
        }
        // Where the path leads: below a folder that could not be listed, inside a game directory, or to nothing.
        const std::string& key = source.key.key();
        const std::optional<catalog::SourceUnavailableReason> unavailable = unavailableAbove(unavailableFolders, key);
        const ScannedItem* gameDirectory = nullptr;
        for (std::size_t slash = key.find('/'); slash != std::string::npos && !unavailable && !gameDirectory;
             slash = key.find('/', slash + 1)) {
            const std::string_view prefix(key.data(), slash);
            for (const ScannedItem& item : listing.sameKey(prefix)) {
                if (item.facts.directory && !item.enteredFolder()) {
                    gameDirectory = &item;
                }
            }
        }
        if (unavailable) {
            addUnavailableEntries(result, sameKey, rules, *unavailable);
            continue;
        }
        const GamelistEntry* lastGame = nullptr;
        for (const GamelistEntry* entry : sameKey) {
            if (gameDirectory != nullptr) {
                entryIssue(kSourceIssueInsideGame, *entry,
                           fmt::format("names something inside \"{}\", a game directory ES-DE never looks into: "
                                       "skipped",
                                       gameDirectory->key.relativePath()));
            } else if (entry->kind == GamelistEntryKind::Folder) {
                entryIssue(kSourceIssueFolderNotFound, *entry, "names no folder: skipped, as ES-DE skips it");
            } else {
                lastGame = entry;
            }
        }
        if (lastGame != nullptr) {
            CatalogEntry entry = entryFrom(system, *lastGame);
            entry.state = EntryState::MissingFile;
            entry.availability = catalog::Availability::Missing;
            entry.title =
                titleOf(entry.metadata, esdeDefaultName(lastGame->key.relativePath(), EsdeNameKind::File, rules));
            result.entries.push_back(std::move(entry));
        }
    }
}

Status Loader::addDiscGroups(SystemResult& result, const RomFolderListing* listing, IssueList& issues) {
    const catalog::SystemName& system = result.info.name;
    const fs::path& folder = result.info.romFolder;
    std::vector<CatalogEntry>& entries = result.entries;

    // The entries by key (several when names differ in case only) and by exact path.
    std::unordered_map<std::string, std::vector<std::size_t>> byKey;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].key.empty()) {
            byKey[entries[i].key.key()].push_back(i);
        }
    }
    // The groups the last import stored for this system, by head key.
    std::unordered_map<std::string, const catalog::CachedDiscGroup*> cached;
    for (const catalog::CachedDiscGroup& group : m_options.discGroups) {
        if (group.group.system == system) {
            cached[group.group.head.key()] = &group;
        }
    }

    // The heads: the games that are cue sheets or playlists, each with the group the last import stored for it.
    struct Head {
        std::size_t entry = 0;
        catalog::DiscGroupKind kind = catalog::DiscGroupKind::Cue;
        const catalog::CachedDiscGroup* stored = nullptr;
        bool read = false; ///< the sheet is read; otherwise the stored group, if any, stands
    };
    std::vector<Head> heads;
    std::map<catalog::LookupKey, std::size_t> indexOf;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const CatalogEntry& entry = entries[i];
        if (!entry.key.empty()) {
            indexOf.emplace(entry.key, i);
        }
        if (entry.state != EntryState::Game || (entry.file && entry.file->directory)) {
            continue;
        }
        const auto kind = discSheetKind(nameOf(entry.key.relativePath()));
        if (!kind) {
            continue;
        }
        const auto cache = cached.find(entry.key.key());
        const catalog::CachedDiscGroup* stored = cache == cached.end() ? nullptr : cache->second;
        // A head whose folder cannot be listed keeps its stored group; one that kept its size and time too.
        const bool listed = entry.availability == catalog::Availability::Available && entry.file.has_value();
        const bool current = stored != nullptr && listed && stored->headSize == entry.file->size &&
                             stored->headLastWriteTime == entry.file->lastWriteTime;
        m_statistics.sheetsFromCache += current ? 1 : 0;
        heads.push_back(Head{i, *kind, stored, listed && !current});
    }

    // The sheets to read, by a few readers at once: on a hard drive their seeks overlap (on the reference machine's
    // external drive about four times faster than one by one). The results are taken in the heads' order below.
    std::vector<std::optional<Expected<DiscSheet>>> sheets(heads.size());
    {
        std::vector<std::size_t> toRead;
        for (std::size_t h = 0; h < heads.size(); ++h) {
            if (heads[h].read) {
                toRead.push_back(h);
            }
        }
        const Clock::time_point start = Clock::now();
        std::atomic<std::size_t> next{0};
        const auto reader = [&] {
            for (std::size_t n = next++; n < toRead.size() && !cancelled(); n = next++) {
                const Head& head = heads[toRead[n]];
                try {
                    sheets[toRead[n]].emplace(readDiscSheet(folder, entries[head.entry].key, head.kind));
                } catch (const std::bad_alloc&) {
                    sheets[toRead[n]].emplace(makeError(ErrorCode::OutOfRange, "out of memory while reading it"));
                }
            }
        };
        std::vector<std::thread> readers;
        for (std::size_t k = 1; k < std::min(kSheetReaders, toRead.size()); ++k) {
            try {
                readers.emplace_back(reader);
            } catch (const std::system_error&) {
                break; // no more threads: the readers there are share the work
            }
        }
        reader();
        for (std::thread& thread : readers) {
            thread.join();
        }
        m_statistics.sheetTime += Clock::now() - start;
        if (cancelled()) {
            return cancelledError();
        }
    }

    // A reference below a folder that could not be listed may name an entry the gamelist alone tells of: not missing.
    const UnavailableKeys unavailableFolders = listing != nullptr ? unavailableKeysOf(*listing) : UnavailableKeys{};
    std::vector<DiscHeadLinks> links;
    for (std::size_t h = 0; h < heads.size(); ++h) {
        const Head& head = heads[h];
        const CatalogEntry& entry = entries[head.entry];
        const std::string headFile = fileText(folder, entry.key.relativePath());
        std::vector<catalog::LookupKey> references; // as the sheet names them, in its order
        const auto takeStored = [&] {
            if (head.stored != nullptr) {
                references = head.stored->group.members;
            }
        };
        if (!head.read) {
            takeStored();
        } else if (const Expected<DiscSheet>& sheet = *sheets[h]; !sheet) {
            ++m_statistics.sheetsUnreadable;
            issues.add(catalog::Issue{
                catalog::IssueSeverity::Warning, std::string(kDiscIssueSheetUnreadable), system.str(), headFile,
                head.stored != nullptr
                    ? fmt::format("cannot be read ({}): the group read before is kept", sheet.error().message)
                    : fmt::format("cannot be read ({}): the files it names stay what they are",
                                  sheet.error().message)});
            takeStored();
        } else {
            ++m_statistics.sheetsRead;
            for (const std::string& reference : sheet->references) {
                auto relative = resolveSheetReference(reference, entry.key.relativePath(), folder);
                auto key = relative ? catalog::makeLookupKey(*relative)
                                    : Expected<catalog::LookupKey>(propagate(relative.error()));
                if (!key) {
                    issues.add(catalog::Issue{
                        catalog::IssueSeverity::Warning, std::string(kDiscIssueReferenceInvalid), system.str(),
                        headFile, fmt::format("its reference \"{}\" is ignored: {}", reference, key.error().message)});
                } else if (listing != nullptr && listing->sameKey(key->key()).empty() &&
                           !unavailableAbove(unavailableFolders, key->key())) {
                    issues.add(catalog::Issue{catalog::IssueSeverity::Warning, std::string(kDiscIssueReferenceMissing),
                                              system.str(), headFile,
                                              fmt::format("names \"{}\", which is not there", *relative)});
                } else {
                    references.push_back(std::move(*key));
                }
            }
        }
        // The members: the games of those keys (the one spelled like the reference first), the head excluded, once.
        DiscHeadLinks link{entry.key, head.kind, {}};
        for (const catalog::LookupKey& reference : references) {
            const auto found = byKey.find(reference.key());
            if (found == byKey.end()) {
                continue;
            }
            std::optional<std::size_t> member;
            for (const std::size_t m : found->second) {
                if (m == head.entry || entries[m].state != EntryState::Game) {
                    continue;
                }
                if (!member || entries[m].key.relativePath() == reference.relativePath()) {
                    member = m;
                }
            }
            if (member &&
                std::find(link.members.begin(), link.members.end(), entries[*member].key) == link.members.end()) {
                link.members.push_back(entries[*member].key);
            }
        }
        links.push_back(std::move(link));
    }

    DiscGrouping grouping = esde::groupDiscs(system, std::move(links), folder);
    issues.addAll(std::move(grouping.issues));
    for (const auto& [member, head] : grouping.memberHeads) {
        CatalogEntry& entry = entries[indexOf.at(member)];
        entry.state = EntryState::DiscMember;
        entry.discHead = head;
    }
    // A head without a gamelist entry of its own shows the metadata of its first member that has some, members first
    // along a chain (a playlist takes it from a cue sheet that took it from a track).
    std::vector<bool> hasMetadata(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        hasMetadata[i] = entries[i].fromGamelist();
    }
    for (const std::size_t g : grouping.membersFirst) {
        const catalog::DiscGroup& group = grouping.groups[g];
        const std::size_t h = indexOf.at(group.head);
        if (hasMetadata[h]) {
            continue;
        }
        for (const catalog::LookupKey& memberKey : group.members) {
            const std::size_t m = indexOf.at(memberKey);
            if (hasMetadata[m]) {
                entries[h].metadata = entries[m].metadata;
                entries[h].title = titleOf(entries[h].metadata, entries[h].title);
                hasMetadata[h] = true;
                break;
            }
        }
    }
    result.groups = std::move(grouping.groups);
    return {};
}

Expected<SystemResult> Loader::loadSystem(const SystemDefinition& definition) {
    SystemResult result;
    IssueList issues;
    SystemsConfiguration one;
    one.systems.push_back(definition);
    std::vector<catalog::Issue> infoIssues;
    std::vector<catalog::SystemInfo> infos = systemInfos(one, m_romRoot, m_profile, infoIssues);
    result.info = std::move(infos.front());
    issues.addAll(std::move(infoIssues));
    const ScanRules rules = rulesOf(definition);

    auto read = readSystemGamelist(definition.name, result.info.romFolder, issues);
    if (!read) {
        return propagate(std::move(read.error()));
    }
    result.info.gamelist = read->state;
    if (read->gamelist) {
        result.info.alternativeEmulator = read->gamelist->alternativeEmulator;
    }
    const Gamelist* gamelist = read->gamelist ? &*read->gamelist : nullptr;
    const fs::path gamelistFile = gamelistPath(m_profile.esdeHome, definition.name);
    const std::string folderText = core::pathToUtf8Lossy(result.info.romFolder);

    std::optional<RomFolderListing> listing;
    if (result.info.availability == catalog::Availability::Available) {
        const Clock::time_point start = Clock::now();
        auto scanned = scanRomFolder(result.info.romFolder, definition.name, rules);
        m_statistics.scanTime += Clock::now() - start;
        if (scanned) {
            ++m_statistics.systemsScanned;
            m_statistics.filesListed += scanned->filesListed;
            m_statistics.foldersListed += scanned->foldersListed;
            listing = std::move(*scanned);
        } else if (scanned.error().code == ErrorCode::Cancelled) {
            return propagate(std::move(scanned.error()));
        } else {
            // The folder went away, or cannot be listed after all.
            result.info.availability = catalog::Availability::SourceUnavailable;
            result.info.unavailableReason = scanned.error().code == ErrorCode::Unavailable
                                                ? catalog::SourceUnavailableReason::NotListable
                                                : catalog::SourceUnavailableReason::FolderMissing;
        }
    }

    if (!listing) {
        // Nothing is known of the folder: the gamelist's entries are kept, source unavailable.
        const bool missing = result.info.unavailableReason == catalog::SourceUnavailableReason::FolderMissing;
        const bool named = gamelist != nullptr && !gamelist->entries.empty();
        if (!missing || named) {
            issues.add(
                catalog::Issue{missing ? catalog::IssueSeverity::Warning : catalog::IssueSeverity::Error,
                               std::string(missing ? kSourceIssueSystemMissing : kSourceIssueSystemUnavailable),
                               definition.name.str(), folderText,
                               fmt::format("the ROM folder is {} ({}){}", missing ? "not there" : "not available",
                                           catalog::toString(result.info.unavailableReason),
                                           named ? ": its gamelist's entries are kept as source unavailable" : "")});
        }
        if (gamelist != nullptr) {
            std::vector<std::string> keysSeen;
            std::unordered_map<std::string, std::vector<const GamelistEntry*>> byKey;
            for (const GamelistEntry& entry : gamelist->entries) {
                if (entry.outsideSystemFolder()) {
                    CatalogEntry outside = entryFrom(definition.name, entry);
                    outside.state = EntryState::OutsideSystemFolder;
                    outside.title = titleOf(outside.metadata, catalog::titleFromFileStem(entry.path));
                    result.entries.push_back(std::move(outside));
                    continue;
                }
                auto& same = byKey[entry.key.key()];
                if (same.empty()) {
                    keysSeen.push_back(entry.key.key());
                }
                same.push_back(&entry);
            }
            for (const std::string& key : keysSeen) {
                addUnavailableEntries(result, byKey[key], rules, result.info.unavailableReason);
            }
        }
        excludeBelowHiddenFolders(result);
        if (Status grouped = addDiscGroups(result, nullptr, issues); !grouped) {
            return propagate(std::move(grouped.error()));
        }
    } else {
        SystemBlock block = SystemBlock::None;
        if (!m_settings.showHiddenFiles && listing->systemFolderHidden) {
            block = SystemBlock::Hidden;
        } else if (!m_settings.parseGamelistOnly && listing->noLoadAtRoot) {
            block = SystemBlock::NoLoad;
        }
        if (block != SystemBlock::None) {
            issues.add(catalog::Issue{catalog::IssueSeverity::Info, std::string(kSourceIssueSystemNotLoaded),
                                      definition.name.str(), folderText,
                                      block == SystemBlock::Hidden
                                          ? "ES-DE does not load the system: its folder is hidden and ShowHiddenFiles "
                                            "is off"
                                          : "ES-DE does not load the system: its folder holds a noload.txt"});
        }
        issues.addAll(std::move(listing->issues));
        result.unavailableFolders = listing->unavailableFolders;
        mergeListing(result, *listing, gamelist, rules, block, gamelistFile, issues);
        excludeBelowHiddenFolders(result);
        if (Status grouped = addDiscGroups(result, &*listing, issues); !grouped) {
            return propagate(std::move(grouped.error()));
        }
    }
    // What ES-DE would list: games and members on disk (not those its gamelist alone tells of below a folder that
    // cannot be listed: ES-DE skips a system whose folder is not there and goes on to the name's next definition).
    for (const CatalogEntry& entry : result.entries) {
        const bool game = entry.state == EntryState::Game || entry.state == EntryState::DiscMember;
        result.esdeGames += game && entry.availability == catalog::Availability::Available ? 1 : 0;
    }
    std::sort(result.entries.begin(), result.entries.end(), catalog::entryOrder);
    result.issues = issues.take();
    return result;
}

Expected<catalog::LibrarySnapshot> Loader::run() {
    const Clock::time_point start = Clock::now();
    const std::string home = core::pathToUtf8Lossy(m_profile.esdeHome);
    const FolderCheck homeCheck = checkFolder(m_profile.esdeHome);
    if (homeCheck.availability != catalog::Availability::Available) {
        return makeError(homeCheck.reason == catalog::SourceUnavailableReason::FolderMissing ? ErrorCode::NotFound
                                                                                             : ErrorCode::Unavailable,
                         "the ES-DE home is no folder that can be read: " + homeCheck.detail, home);
    }
    auto settings = readSettings(m_profile.esdeHome);
    if (!settings) {
        return propagate(std::move(settings.error()));
    }
    m_settings = std::move(*settings);
    m_issues.insert(m_issues.end(), m_settings.issues.begin(), m_settings.issues.end());
    if (m_settings.legacyGamelistFileLocation) {
        m_issues.push_back(catalog::Issue{catalog::IssueSeverity::Warning,
                                          std::string(kSourceIssueLegacyGamelists),
                                          {},
                                          core::pathToUtf8Lossy(m_settings.file),
                                          "LegacyGamelistFileLocation is on: ES-DE reads a gamelist.xml inside a ROM "
                                          "folder first, Retroverse only <ES-DE home>\\gamelists"});
    }
    m_romRoot = resolveRomRoot(m_settings, m_profile);
    m_issues.insert(m_issues.end(), m_romRoot.issues.begin(), m_romRoot.issues.end());
    auto systems = loadSystems(m_profile);
    if (!systems) {
        return propagate(std::move(systems.error()));
    }
    m_issues.insert(m_issues.end(), systems->issues.begin(), systems->issues.end());
    for (const std::string& name : m_sourceOptions.systems) {
        if (systems->find(name) == nullptr) {
            return makeError(ErrorCode::InvalidArgument, "the configuration defines no system of that name", name);
        }
    }
    m_statistics.configurationTime = Clock::now() - start;

    catalog::LibrarySnapshot snapshot;
    snapshot.profile = m_profile;
    for (const SystemDefinition& definition : systems->systems) {
        const std::vector<std::string>& only = m_sourceOptions.systems;
        if (!only.empty() && std::find(only.begin(), only.end(), definition.name.str()) == only.end()) {
            continue;
        }
        if (cancelled()) {
            return cancelledError();
        }
        // ES-DE loads the first definition of a name whose folder gives a game, else none: the first stands for it.
        auto chosen = loadSystem(definition);
        if (!chosen) {
            return propagate(std::move(chosen.error()));
        }
        if (chosen->esdeGames == 0) {
            for (const SystemDefinition& fallback : systems->fallbacks) {
                if (fallback.name != definition.name) {
                    continue;
                }
                auto tried = loadSystem(fallback);
                if (!tried) {
                    return propagate(std::move(tried.error()));
                }
                if (tried->esdeGames > 0) {
                    tried->issues.insert(
                        tried->issues.begin(),
                        catalog::Issue{catalog::IssueSeverity::Info, std::string(kSourceIssueFallbackUsed),
                                       definition.name.str(), core::pathToUtf8Lossy(tried->info.romFolder),
                                       fmt::format("the first definition of {} gives no game: ES-DE loads a later one "
                                                   "(its path \"{}\")",
                                                   definition.name.str(), fallback.path)});
                    chosen = std::move(tried);
                    break;
                }
            }
        }
        SystemResult& result = *chosen;
        snapshot.systems.push_back(std::move(result.info));
        std::move(result.entries.begin(), result.entries.end(), std::back_inserter(snapshot.entries));
        std::move(result.groups.begin(), result.groups.end(), std::back_inserter(snapshot.discGroups));
        std::move(result.unavailableFolders.begin(), result.unavailableFolders.end(),
                  std::back_inserter(snapshot.unavailableFolders));
        m_issues.insert(m_issues.end(), std::make_move_iterator(result.issues.begin()),
                        std::make_move_iterator(result.issues.end()));
    }
    snapshot.issues = std::move(m_issues);
    m_statistics.systemsConfigured = snapshot.systems.size();
    m_statistics.totalTime = Clock::now() - start;
    spdlog::info("ES-DE library '{}' read: {} system(s), {} entr(ies), {} disc group(s), {} unavailable folder(s), "
                 "{} issue(s) in {:.2f} s",
                 m_profile.id.str(), snapshot.systems.size(), snapshot.entries.size(), snapshot.discGroups.size(),
                 snapshot.unavailableFolders.size(), snapshot.issues.size(),
                 std::chrono::duration<double>(m_statistics.totalTime).count());
    return snapshot;
}

} // namespace

EsdeLibrarySource::EsdeLibrarySource(EsdeLibrarySourceOptions options) : m_options(std::move(options)) {}

Expected<catalog::LibrarySnapshot> EsdeLibrarySource::load(const catalog::LibraryProfile& profile,
                                                           const catalog::LibraryLoadOptions& options) {
    m_statistics = EsdeLoadStatistics{};
    try {
        Loader loader(profile, options, m_options, m_statistics);
        return loader.run();
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfRange, "out of memory while reading the library",
                         core::pathToUtf8Lossy(profile.esdeHome));
    }
}

} // namespace rv::esde
