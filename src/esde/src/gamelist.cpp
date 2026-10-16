#include <rv/esde/gamelist.hpp>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/relative_path.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "xml.hpp"

namespace rv::esde {
namespace {

namespace fs = std::filesystem;
using catalog::IssueSeverity;

/// The most bytes of a value or path a message quotes: a field may be megabytes long.
constexpr std::size_t kQuotedBytes = 120;

/// `text` (UTF-8) in quotes for a message: control characters and DEL written as \xHH, cut after kQuotedBytes at a
/// character boundary ("..." marks the cut).
std::string inQuotes(std::string_view text) {
    std::size_t end = text.size();
    const bool cut = end > kQuotedBytes;
    if (cut) {
        end = kQuotedBytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
            --end; // back to the start of the character the cut falls in
        }
    }
    std::string out = "\"";
    for (const char c : text.substr(0, end)) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            out += fmt::format("\\x{:02X}", byte);
        } else {
            out += c;
        }
    }
    out += cut ? "...\"" : "\"";
    return out;
}

/// What ES-DE reads from an element (pugixml's text(): its first text or CDATA child, decoded); empty for an element
/// without one, and for a missing element.
std::string textOf(pugi::xml_node element) {
    return element.text().get();
}

/// Why core::resolveRelativePath() refused a path: its message after "refused: " (the message quotes the whole path
/// first; the issues quote it cut).
std::string_view whyRefused(const Error& error) {
    constexpr std::string_view kMarker = "refused: ";
    const std::string_view message = error.message;
    const std::size_t at = message.rfind(kMarker);
    return at == std::string_view::npos ? message : message.substr(at + kMarker.size());
}

/// What an unreadable value of a normalized element becomes, as ES-DE reads it.
std::string_view unreadableOutcome(std::string_view name) {
    if (name == "favorite" || name == "hidden" || name == "kidgame" || name == "completed" || name == "broken") {
        return "read as false, as ES-DE reads anything but \"true\"";
    }
    if (name == "playcount" || name == "playtime") {
        return "read as 0";
    }
    return "without a value";
}

/// Elements or values counted into one issue: where the issue is in the list, how many, and the first.
struct Tally {
    std::size_t issue = 0;
    std::size_t count = 0;
    std::size_t firstLine = 0;
    /// The first value, quoted (values only).
    std::string firstValue;
};

/// One reading of a parsed gamelist (readGamelist()).
class GamelistReader {
public:
    GamelistReader(const detail::XmlDocument& document, const catalog::SystemName& system, const fs::path& romFolder)
        : m_document(document), m_system(system), m_romFolder(romFolder) {}

    /// Reads the document, whose first <gameList> is `gameList`.
    Gamelist read(pugi::xml_node gameList) {
        for (const std::string& warning : m_document.warnings()) {
            addIssue(IssueSeverity::Warning, kGamelistIssueReadLeniently, warning);
        }
        pugi::xml_node alternativeEmulator;
        for (const pugi::xml_node element : m_document.document().children()) {
            if (element.type() != pugi::node_element) {
                continue;
            }
            const std::string_view name = element.name();
            if (element == gameList) {
                readGameList(gameList);
            } else if (name == "alternativeEmulator" && !alternativeEmulator) {
                alternativeEmulator = element;
                readAlternativeEmulator(element);
            } else {
                ignored(fmt::format("top-level <{}> element(s) besides the first <gameList> and the first "
                                    "<alternativeEmulator>, which ES-DE ignores",
                                    name),
                        element);
            }
        }
        finish();
        return std::move(m_result);
    }

private:
    void readAlternativeEmulator(pugi::xml_node element) {
        const pugi::xml_node label = element.child("label");
        for (const pugi::xml_node child : element.children()) {
            if (child.type() == pugi::node_element && child != label) {
                ignored(fmt::format("<{}> element(s) inside <alternativeEmulator> besides its first <label>, which "
                                    "ES-DE ignores",
                                    child.name()),
                        child);
            }
        }
        m_result.alternativeEmulator = textOf(label);
        if (m_result.alternativeEmulator.empty()) {
            ignored("<alternativeEmulator> element(s) without a <label> text, which ES-DE ignores", element);
        }
    }

    void readGameList(pugi::xml_node gameList) {
        for (const pugi::xml_node element : gameList.children()) {
            if (element.type() != pugi::node_element) {
                continue;
            }
            const std::string_view name = element.name();
            if (name == "game") {
                readEntry(element, GamelistEntryKind::Game);
            } else if (name == "folder") {
                readEntry(element, GamelistEntryKind::Folder);
            } else {
                ignored(fmt::format("<{}> element(s) inside <gameList> besides <game> and <folder>, which ES-DE "
                                    "ignores",
                                    name),
                        element);
            }
        }
    }

    void readEntry(pugi::xml_node element, GamelistEntryKind kind) {
        GamelistEntry entry;
        entry.kind = kind;
        entry.line = m_document.line(element);
        // ES-DE reads the first <path> (pugixml's child()) and skips an entry whose path is empty.
        entry.path = textOf(element.child("path"));
        if (entry.path.empty()) {
            addIssue(IssueSeverity::Warning, kGamelistIssueEntryWithoutPath,
                     fmt::format("the <{}> entry at line {} has no <path>: skipped, as ES-DE skips it", toString(kind),
                                 entry.line));
            return;
        }
        const auto relative = core::resolveRelativePath(entry.path, m_romFolder);
        if (relative) {
            auto key = catalog::makeLookupKey(*relative);
            if (!key) {
                addIssue(IssueSeverity::Warning, kGamelistIssuePathMalformed,
                         fmt::format("the <{}> entry at line {} is skipped: its path {} makes no lookup key ({})",
                                     toString(kind), entry.line, inQuotes(entry.path), key.error().message));
                return;
            }
            entry.key = std::move(*key);
        } else if (relative.error().code == ErrorCode::OutOfRange) {
            addIssue(IssueSeverity::Warning, kGamelistIssuePathOutsideFolder,
                     fmt::format("the <{}> entry at line {} lies outside the system's ROM folder: its path {} is "
                                 "refused ({}); kept as an entry outside the system folder, never indexed",
                                 toString(kind), entry.line, inQuotes(entry.path), whyRefused(relative.error())));
        } else {
            addIssue(IssueSeverity::Warning, kGamelistIssuePathMalformed,
                     fmt::format("the <{}> entry at line {} is skipped: its path {} is malformed ({})", toString(kind),
                                 entry.line, inQuotes(entry.path), whyRefused(relative.error())));
            return;
        }
        supersedeEarlier(entry);
        readFields(element, entry);
        m_superseded.push_back(false);
        m_result.entries.push_back(std::move(entry));
    }

    /// An earlier entry of the same lookup key (outside the folder: of the same path) is superseded by `entry`, which
    /// is about to be kept.
    void supersedeEarlier(const GamelistEntry& entry) {
        auto& index = entry.outsideSystemFolder() ? m_outsideByPath : m_byKey;
        const auto [found, added] =
            index.try_emplace(entry.outsideSystemFolder() ? entry.path : entry.key.key(), m_result.entries.size());
        if (added) {
            return;
        }
        const GamelistEntry& earlier = m_result.entries[found->second];
        m_superseded[found->second] = true;
        // ES-DE applies the entries of a path in turn (the last one's metadata shows) but shows two entries for two
        // spellings that differ in case only.
        const std::string_view same = earlier.path == entry.path ? "path: the last one counts, as in ES-DE"
                                      : earlier.key.relativePath() == entry.key.relativePath()
                                          ? "file: the last one counts, as in ES-DE"
                                          : "file (names compared as the file system compares them): the last one "
                                            "counts (ES-DE would show both)";
        addIssue(IssueSeverity::Warning, kGamelistIssueDuplicatePath,
                 fmt::format("the <{}> entry at line {} ({}) is superseded by the <{}> entry at line {} ({}), which "
                             "names the same {}",
                             toString(earlier.kind), earlier.line, inQuotes(earlier.path), toString(entry.kind),
                             entry.line, inQuotes(entry.path), same));
        found->second = m_result.entries.size();
    }

    /// The elements of an entry, in document order: the first of each modelled one read, the others kept as written.
    void readFields(pugi::xml_node element, GamelistEntry& entry) {
        std::vector<std::string_view> met; // the modelled elements met so far
        for (const pugi::xml_node child : element.children()) {
            if (child.type() != pugi::node_element) {
                continue;
            }
            const std::string_view name = child.name();
            if (!catalog::isModelledGamelistElement(name)) {
                entry.raw.unknownElements.push_back({std::string(name), textOf(child)});
                continue;
            }
            if (std::find(met.begin(), met.end(), name) != met.end()) {
                ignored(fmt::format("repeated <{}> element(s) in an entry, which ES-DE ignores (it reads the first)",
                                    name),
                        child);
                continue;
            }
            met.push_back(name);
            std::string text = textOf(child);
            if (name == "path") {
                continue; // resolved by readEntry()
            } else if (name == "name") {
                entry.metadata.name = std::move(text);
            } else if (name == "sortname") {
                entry.metadata.sortName = std::move(text);
            } else if (name == "desc") {
                entry.metadata.description = std::move(text);
            } else if (name == "developer") {
                entry.metadata.developer = std::move(text);
            } else if (name == "publisher") {
                entry.metadata.publisher = std::move(text);
            } else if (name == "genre") {
                entry.metadata.genre = std::move(text);
            } else if (name == "altemulator") {
                entry.altEmulator = std::move(text);
            } else {
                readNormalized(name, std::move(text), child, entry);
            }
        }
    }

    /// A normalized element (catalog::isNormalizedGamelistElement()): its original form kept, its value parsed.
    void readNormalized(std::string_view name, std::string text, pugi::xml_node element, GamelistEntry& entry) {
        entry.raw.originalForms.push_back({std::string(name), text});
        if (text.empty()) {
            return; // an element without text reads as a missing one, as in ES-DE
        }
        const auto set = [&](auto parsed, auto& target) {
            if (parsed) {
                target = *parsed;
            } else {
                unreadable(name, parsed.error().message, text, element);
            }
        };
        catalog::EsdeFlags& flags = entry.flags;
        if (name == "releasedate") {
            set(catalog::parseReleaseDate(text), entry.metadata.releaseDate);
        } else if (name == "rating") {
            set(catalog::parseRating(text), entry.metadata.rating);
        } else if (name == "players") {
            set(catalog::parsePlayers(text), entry.metadata.players);
        } else if (name == "favorite") {
            set(catalog::parseEsdeBool(text), flags.favorite);
        } else if (name == "hidden") {
            set(catalog::parseEsdeBool(text), flags.hidden);
        } else if (name == "kidgame") {
            set(catalog::parseEsdeBool(text), flags.kidGame);
        } else if (name == "completed") {
            set(catalog::parseEsdeBool(text), flags.completed);
        } else if (name == "broken") {
            set(catalog::parseEsdeBool(text), flags.broken);
        } else if (name == "playcount") {
            set(catalog::parseEsdeCount(text), flags.playCount);
        } else if (name == "playtime") {
            set(catalog::parseEsdeCount(text), flags.playTimeSeconds);
        } else if (name == "lastplayed") {
            set(catalog::parseEsdeDateTime(text), flags.lastPlayed);
        }
    }

    void addIssue(IssueSeverity severity, std::string_view code, std::string reason) {
        m_result.issues.push_back(
            catalog::Issue{severity, std::string(code), m_system.str(), m_document.source(), std::move(reason)});
    }

    /// Counts one more occurrence of `what` into its issue, which its first occurrence makes.
    Tally& tally(IssueSeverity severity, std::string_view code, const std::string& what, pugi::xml_node element) {
        const auto [found, added] = m_tallies.try_emplace(what);
        Tally& counted = found->second;
        if (added) {
            counted.issue = m_result.issues.size();
            counted.firstLine = m_document.line(element);
            addIssue(severity, code, {});
        }
        ++counted.count;
        return counted;
    }

    void ignored(const std::string& what, pugi::xml_node element) {
        tally(IssueSeverity::Info, kGamelistIssueIgnoredElement, what, element);
    }

    void unreadable(std::string_view name, std::string_view problem, std::string_view value, pugi::xml_node element) {
        Tally& counted = tally(IssueSeverity::Info, kGamelistIssueValueUnreadable,
                               fmt::format("<{}> value(s) that are {}: kept as written, {}", name, problem,
                                           unreadableOutcome(name)),
                               element);
        if (counted.count == 1) {
            counted.firstValue = inQuotes(value);
        }
    }

    /// The counted issues' reasons; the superseded entries removed.
    void finish() {
        for (const auto& [what, counted] : m_tallies) {
            m_result.issues[counted.issue].reason =
                counted.firstValue.empty()
                    ? fmt::format("{} {}; the first at line {}", counted.count, what, counted.firstLine)
                    : fmt::format("{} {}; the first, {}, at line {}", counted.count, what, counted.firstValue,
                                  counted.firstLine);
        }
        std::vector<GamelistEntry> kept;
        kept.reserve(m_result.entries.size());
        for (std::size_t i = 0; i < m_result.entries.size(); ++i) {
            if (!m_superseded[i]) {
                kept.push_back(std::move(m_result.entries[i]));
            }
        }
        m_result.entries = std::move(kept);
    }

    const detail::XmlDocument& m_document;
    const catalog::SystemName& m_system;
    const fs::path& m_romFolder;
    Gamelist m_result;
    /// Per entry of m_result.entries: superseded by a later one, removed at the end.
    std::vector<bool> m_superseded;
    /// The entry kept so far for each lookup key, and for each path outside the ROM folder (as written).
    std::unordered_map<std::string, std::size_t> m_byKey;
    std::unordered_map<std::string, std::size_t> m_outsideByPath;
    /// The counted issues, by what they count.
    std::map<std::string, Tally> m_tallies;
};

} // namespace

fs::path gamelistPath(const fs::path& esdeHome, const catalog::SystemName& system) {
    // A SystemName is well-formed UTF-8 (SystemName::parse() checks it), which pathFromUtf8() never throws on.
    return esdeHome / "gamelists" / core::pathFromUtf8(system.str()) / "gamelist.xml";
}

Expected<Gamelist> readGamelist(const fs::path& file, const catalog::SystemName& system, const fs::path& romFolder,
                                std::uintmax_t maxBytes) {
    if (!romFolder.is_absolute()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the ROM folder a gamelist's paths are resolved against is not absolute: " +
                             core::pathToUtf8Lossy(romFolder),
                         core::pathToUtf8Lossy(file));
    }
    auto document = detail::loadXmlFile(file, maxBytes);
    if (!document) {
        return propagate(std::move(document.error()));
    }
    const pugi::xml_node gameList = document->topLevel("gameList");
    if (!gameList) {
        return makeError(ErrorCode::ParseError, "no <gameList> element: not a gamelist, or one that ES-DE is writing",
                         document->source());
    }
    Gamelist gamelist = GamelistReader(*document, system, romFolder).read(gameList);
    spdlog::debug("Gamelist {}: {} game(s), {} folder(s), {} issue(s), alternative emulator \"{}\"",
                  document->source(), gamelist.count(GamelistEntryKind::Game),
                  gamelist.count(GamelistEntryKind::Folder), gamelist.issues.size(), gamelist.alternativeEmulator);
    return gamelist;
}

} // namespace rv::esde
