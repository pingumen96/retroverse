#include <rv/esde/gamelist.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <numeric>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/relative_path.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "xml.hpp"

namespace rv::esde {
namespace {

namespace fs = std::filesystem;
using catalog::IssueSeverity;

/// The most bytes of a value, path or element name a message quotes: a field may be megabytes long.
constexpr std::size_t kQuotedBytes = 120;

/// The start of `text` a message shows: all of it, or its first kQuotedBytes cut at a character boundary (`cut` then
/// true).
std::string_view shownPart(std::string_view text, bool& cut) noexcept {
    cut = text.size() > kQuotedBytes;
    if (!cut) {
        return text;
    }
    std::size_t end = kQuotedBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
        --end; // back to the start of the character the cut falls in
    }
    return text.substr(0, end);
}

/// `text` (UTF-8) in quotes for a message: control characters and DEL written as \xHH, cut after kQuotedBytes at a
/// character boundary ("..." marks the cut).
std::string inQuotes(std::string_view text) {
    bool cut = false;
    std::string out = "\"";
    for (const char c : shownPart(text, cut)) {
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

/// An element's name for a message, "<name>", cut like a quoted value ("<name...>"); XML names hold no control
/// characters.
std::string elementName(std::string_view name) {
    bool cut = false;
    const std::string_view shown = shownPart(name, cut);
    return fmt::format("<{}{}>", shown, cut ? "..." : "");
}

/// The UTF-16 code units of `text`, well-formed UTF-8 as every value the XML loader gives: one per character, two for
/// a character beyond U+FFFF (a four-byte sequence).
std::size_t utf16Length(std::string_view text) noexcept {
    std::size_t units = 0;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        units += (byte & 0xC0u) != 0x80u ? 1u : 0u; // a character starts here
        units += byte >= 0xF0u ? 1u : 0u;           // and takes a surrogate pair
    }
    return units;
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

/// The issues about single entries, each code listed up to kGamelistIssuesListedPerCode times.
enum class EntryIssue : std::uint8_t { WithoutPath, PathMalformed, PathOutsideFolder, DuplicatePath };
constexpr std::size_t kEntryIssueKinds = 4;

constexpr std::string_view codeOf(EntryIssue which) noexcept {
    switch (which) {
    case EntryIssue::WithoutPath:
        return kGamelistIssueEntryWithoutPath;
    case EntryIssue::PathMalformed:
        return kGamelistIssuePathMalformed;
    case EntryIssue::PathOutsideFolder:
        return kGamelistIssuePathOutsideFolder;
    case EntryIssue::DuplicatePath:
        return kGamelistIssueDuplicatePath;
    }
    return kGamelistIssueDuplicatePath;
}

/// How many issues of a code about single entries were met, and the line of the last one not listed.
struct EntryIssueCount {
    std::size_t met = 0;
    std::size_t lastLine = 0;
};

/// What a counted issue counts: one issue per kind and element name, whose reason finish() writes.
enum class Counted : std::uint8_t {
    TopLevel,              ///< top-level elements besides the first <gameList> and the first <alternativeEmulator>
    InGameList,            ///< elements inside <gameList> besides <game> and <folder>
    InAlternativeEmulator, ///< elements inside <alternativeEmulator> besides its first <label>
    WithoutLabel,          ///< <alternativeEmulator> elements without a label text (no name)
    Repeated,              ///< modelled elements repeated within an entry
    BeyondUnknownLimit,    ///< unknown elements of an entry beyond kGamelistUnknownElementsPerEntry (no name)
    Unreadable,            ///< values of a normalized element that do not parse
};
constexpr std::size_t kCountedKinds = static_cast<std::size_t>(Counted::Unreadable) + 1;

/// The kinds whose elements can bear any name: past kGamelistNamesCountedPerKind names, the others are counted together
/// (the other kinds' names are ES-DE's, a few dozen at most).
constexpr bool bearsAnyName(Counted kind) noexcept {
    return kind == Counted::TopLevel || kind == Counted::InGameList || kind == Counted::InAlternativeEmulator;
}

/// Elements or values counted into one issue.
struct Tally {
    Counted kind = Counted::TopLevel;
    /// The elements' name; empty for a kind that names none and for the elements of other names.
    std::string name;
    /// The elements of every name beyond the first kGamelistNamesCountedPerKind of their kind.
    bool otherNames = false;
    /// Where the issue is in the list, how many, and the line of the first.
    std::size_t issue = 0;
    std::size_t count = 0;
    std::size_t firstLine = 0;
    /// The first value, quoted (Unreadable), or the first element's name (other names).
    std::string firstValue;
    /// Unreadable: what the values are not (the normalizer's message, the same for every value of a name).
    std::string problem;
};

/// The places in Gamelist::entries of the entries kept so far for one lookup key: one, unless they differ in kind or in
/// the case of their path.
struct KeySlots {
    std::size_t first = 0;
    std::vector<std::size_t> more;
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
                count(Counted::TopLevel, name, element);
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
                count(Counted::InAlternativeEmulator, child.name(), child);
            }
        }
        m_result.alternativeEmulator = textOf(label);
        if (m_result.alternativeEmulator.empty()) {
            count(Counted::WithoutLabel, {}, element);
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
                count(Counted::InGameList, name, element);
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
            entryIssue(EntryIssue::WithoutPath, entry.line, [&] {
                return fmt::format("the <{}> entry at line {} has no <path>: skipped, as ES-DE skips it",
                                   toString(kind), entry.line);
            });
            return;
        }
        // Before resolving, which keeps every element of the path folded: dozens of bytes per byte of a huge path.
        if (const std::size_t units = utf16Length(entry.path); units > kGamelistMaxPathUtf16Units) {
            entryIssue(EntryIssue::PathMalformed, entry.line, [&] {
                return fmt::format("the <{}> entry at line {} is skipped: its path {} is longer than any path Windows "
                                   "can name ({} UTF-16 code units, at most {})",
                                   toString(kind), entry.line, inQuotes(entry.path), units, kGamelistMaxPathUtf16Units);
            });
            return;
        }
        const auto relative = core::resolveRelativePath(entry.path, m_romFolder);
        if (relative) {
            auto key = catalog::makeLookupKey(*relative);
            if (!key) {
                entryIssue(EntryIssue::PathMalformed, entry.line, [&] {
                    return fmt::format("the <{}> entry at line {} is skipped: its path {} makes no lookup key ({})",
                                       toString(kind), entry.line, inQuotes(entry.path), key.error().message);
                });
                return;
            }
            entry.key = std::move(*key);
        } else if (relative.error().code == ErrorCode::OutOfRange) {
            entryIssue(EntryIssue::PathOutsideFolder, entry.line, [&] {
                return fmt::format("the <{}> entry at line {} lies outside the system's ROM folder: its path {} is "
                                   "refused ({}); kept as an entry outside the system folder, never indexed",
                                   toString(kind), entry.line, inQuotes(entry.path), whyRefused(relative.error()));
            });
        } else {
            entryIssue(EntryIssue::PathMalformed, entry.line, [&] {
                return fmt::format("the <{}> entry at line {} is skipped: its path {} is malformed ({})",
                                   toString(kind), entry.line, inQuotes(entry.path), whyRefused(relative.error()));
            });
            return;
        }
        const std::size_t slot = placeOf(entry);
        readFields(element, entry);
        store(slot, std::move(entry));
    }

    /// Where `entry`, about to be kept, goes in m_result.entries, its duplicates reported: the place of the earlier
    /// entry it supersedes (of the same kind and relative path; outside the folder, of the same path as written), else
    /// a new place at the end. An earlier entry of the same key but of another kind or spelling stays: which one ES-DE
    /// applies depends on the disk (entryForListed()).
    std::size_t placeOf(const GamelistEntry& entry) {
        const std::size_t end = m_result.entries.size();
        if (entry.outsideSystemFolder()) {
            const auto [found, added] = m_outsideByPath.try_emplace(entry.path, end);
            if (!added) {
                superseded(m_result.entries[found->second], entry, "the last one is kept");
            }
            return found->second;
        }
        const auto [found, added] = m_byKey.try_emplace(entry.key.key(), KeySlots{end, {}});
        if (added) {
            return end;
        }
        KeySlots& slots = found->second;
        std::size_t latest = slots.first; // the entry of the key read last, which the report names
        for (std::size_t i = 0; i <= slots.more.size(); ++i) {
            const std::size_t slot = i == 0 ? slots.first : slots.more[i - 1];
            const GamelistEntry& earlier = m_result.entries[slot];
            if (earlier.kind == entry.kind && earlier.key.relativePath() == entry.key.relativePath()) {
                superseded(earlier, entry, "the last one counts, as in ES-DE");
                return slot;
            }
            latest = m_order[slot] > m_order[latest] ? slot : latest;
        }
        // ES-DE applies an entry to the file spelled exactly like it, and only when their kinds match: only the disk
        // tells which of the two applies.
        entryIssue(EntryIssue::DuplicatePath, entry.line, [&] {
            const GamelistEntry& earlier = m_result.entries[latest];
            return fmt::format("the <{}> entry at line {} ({}) names the same {} as the <{}> entry at line {} ({}): "
                               "both kept, for the ROM scan to choose by what is on disk",
                               toString(entry.kind), entry.line, inQuotes(entry.path), sameWhat(earlier, entry),
                               toString(earlier.kind), earlier.line, inQuotes(earlier.path));
        });
        slots.more.push_back(end);
        return end;
    }

    /// What two entries of one file have in common, for the messages.
    static std::string_view sameWhat(const GamelistEntry& earlier, const GamelistEntry& later) {
        if (earlier.path == later.path) {
            return "path";
        }
        return earlier.key.relativePath() == later.key.relativePath()
                   ? "file"
                   : "file (names compared as the file system compares them)";
    }

    void superseded(const GamelistEntry& earlier, const GamelistEntry& later, std::string_view outcome) {
        entryIssue(EntryIssue::DuplicatePath, later.line, [&] {
            return fmt::format("the <{}> entry at line {} ({}) is superseded by the <{}> entry at line {} ({}), which "
                               "names the same {}: {}",
                               toString(earlier.kind), earlier.line, inQuotes(earlier.path), toString(later.kind),
                               later.line, inQuotes(later.path), sameWhat(earlier, later), outcome);
        });
    }

    /// Keeps `entry` in the place placeOf() gave it.
    void store(std::size_t slot, GamelistEntry entry) {
        const std::size_t order = m_stored++;
        if (slot == m_result.entries.size()) {
            m_result.entries.push_back(std::move(entry));
            m_order.push_back(order);
            return;
        }
        // In the place of the entry it supersedes, which is released at once; finish() restores document order.
        m_reordered = m_reordered || slot + 1 != m_result.entries.size();
        m_result.entries[slot] = std::move(entry);
        m_order[slot] = order;
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
                if (entry.raw.unknownElements.size() < kGamelistUnknownElementsPerEntry) {
                    entry.raw.unknownElements.push_back({std::string(name), textOf(child)});
                } else {
                    count(Counted::BeyondUnknownLimit, {}, child);
                }
                continue;
            }
            if (std::find(met.begin(), met.end(), name) != met.end()) {
                count(Counted::Repeated, name, child);
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

    /// Lists a warning about the entry at `line`, unless kGamelistIssuesListedPerCode issues of its code are listed
    /// already: it is then only counted, into the issue finish() adds. `reason()` writes its text when it is listed.
    template <typename Reason>
    void entryIssue(EntryIssue which, std::size_t line, const Reason& reason) {
        EntryIssueCount& counted = m_entryIssues[static_cast<std::size_t>(which)];
        if (++counted.met <= kGamelistIssuesListedPerCode) {
            addIssue(IssueSeverity::Warning, codeOf(which), reason());
        } else {
            counted.lastLine = line;
        }
    }

    /// Counts one more element of `kind` named `name` (empty for the kinds that name none) into its issue, which the
    /// first one makes; past kGamelistNamesCountedPerKind names of a kind that bears any name, the elements of further
    /// names are counted together.
    Tally& count(Counted kind, std::string_view name, pugi::xml_node element) {
        std::vector<Tally>& tallies = m_tallies[static_cast<std::size_t>(kind)];
        for (Tally& tally : tallies) {
            if (!tally.otherNames && tally.name == name) {
                ++tally.count;
                return tally;
            }
        }
        if (bearsAnyName(kind) && tallies.size() >= kGamelistNamesCountedPerKind) {
            if (tallies.size() == kGamelistNamesCountedPerKind) {
                Tally& other = add(kind, {}, element);
                other.otherNames = true;
                other.firstValue = elementName(name);
            }
            Tally& other = tallies.back();
            ++other.count;
            return other;
        }
        Tally& added = add(kind, name, element);
        ++added.count;
        return added;
    }

    /// A new tally and its issue, whose reason finish() writes.
    Tally& add(Counted kind, std::string_view name, pugi::xml_node element) {
        Tally tally;
        tally.kind = kind;
        tally.name = std::string(name);
        tally.issue = m_result.issues.size();
        tally.firstLine = m_document.line(element);
        addIssue(IssueSeverity::Info,
                 kind == Counted::Unreadable ? kGamelistIssueValueUnreadable : kGamelistIssueIgnoredElement, {});
        std::vector<Tally>& tallies = m_tallies[static_cast<std::size_t>(kind)];
        tallies.push_back(std::move(tally));
        return tallies.back();
    }

    void unreadable(std::string_view name, std::string_view problem, std::string_view value, pugi::xml_node element) {
        Tally& counted = count(Counted::Unreadable, name, element);
        if (counted.count == 1) {
            counted.problem = std::string(problem);
            counted.firstValue = inQuotes(value);
        }
    }

    /// The reason of a counted issue.
    static std::string reasonOf(const Tally& tally) {
        const std::string elements =
            tally.otherNames ? std::string("element(s) of other names") : elementName(tally.name) + " element(s)";
        std::string what;
        switch (tally.kind) {
        case Counted::TopLevel:
            what = "top-level " + elements +
                   " besides the first <gameList> and the first <alternativeEmulator>, which ES-DE ignores";
            break;
        case Counted::InGameList:
            what = elements + " inside <gameList> besides <game> and <folder>, which ES-DE ignores";
            break;
        case Counted::InAlternativeEmulator:
            what = elements + " inside <alternativeEmulator> besides its first <label>, which ES-DE ignores";
            break;
        case Counted::WithoutLabel:
            what = "<alternativeEmulator> element(s) without a <label> text, which ES-DE ignores";
            break;
        case Counted::Repeated:
            what = "repeated " + elements + " in an entry, which ES-DE ignores (it reads the first)";
            break;
        case Counted::BeyondUnknownLimit:
            what = fmt::format("unknown element(s) of an entry beyond its first {}, which ES-DE ignores and Retroverse "
                               "does not keep",
                               kGamelistUnknownElementsPerEntry);
            break;
        case Counted::Unreadable:
            what = fmt::format("{} value(s) that are {}: kept as written, {}", elementName(tally.name), tally.problem,
                               unreadableOutcome(tally.name));
            break;
        }
        return tally.firstValue.empty() ? fmt::format("{} {}; the first at line {}", tally.count, what, tally.firstLine)
                                        : fmt::format("{} {}; the first, {}, at line {}", tally.count, what,
                                                      tally.firstValue, tally.firstLine);
    }

    /// The counted issues' reasons, the counts of the issues not listed one by one, the entries in document order.
    void finish() {
        for (const std::vector<Tally>& tallies : m_tallies) {
            for (const Tally& tally : tallies) {
                m_result.issues[tally.issue].reason = reasonOf(tally);
            }
        }
        for (std::size_t i = 0; i < kEntryIssueKinds; ++i) {
            const EntryIssueCount& counted = m_entryIssues[i];
            if (counted.met > kGamelistIssuesListedPerCode) {
                addIssue(IssueSeverity::Warning, codeOf(static_cast<EntryIssue>(i)),
                         fmt::format("{} more issue(s) of this kind, not listed one by one after the first {}; the "
                                     "last at line {}",
                                     counted.met - kGamelistIssuesListedPerCode, kGamelistIssuesListedPerCode,
                                     counted.lastLine));
            }
        }
        if (m_reordered) {
            restoreDocumentOrder();
        }
    }

    /// Puts the entries back in document order (m_order) after superseding entries took the places of earlier ones:
    /// each entry moves once, along the cycles of the permutation.
    void restoreDocumentOrder() {
        std::vector<GamelistEntry>& entries = m_result.entries;
        std::vector<std::size_t> from(entries.size()); // the place of the entry that goes to each place
        std::iota(from.begin(), from.end(), std::size_t{0});
        std::sort(from.begin(), from.end(), [&](std::size_t a, std::size_t b) { return m_order[a] < m_order[b]; });
        std::vector<bool> done(entries.size(), false);
        for (std::size_t start = 0; start < entries.size(); ++start) {
            if (done[start]) {
                continue;
            }
            GamelistEntry carried = std::move(entries[start]);
            std::size_t place = start;
            while (from[place] != start) {
                entries[place] = std::move(entries[from[place]]);
                done[place] = true;
                place = from[place];
            }
            entries[place] = std::move(carried);
            done[place] = true;
        }
    }

    const detail::XmlDocument& m_document;
    const catalog::SystemName& m_system;
    const fs::path& m_romFolder;
    Gamelist m_result;
    /// Per entry of m_result.entries: when it was read among the entries kept (document order), and whether an entry
    /// took the place of one read before another kept entry.
    std::vector<std::size_t> m_order;
    std::size_t m_stored = 0;
    bool m_reordered = false;
    /// The entries kept so far by lookup key, and by path as written outside the ROM folder.
    std::unordered_map<std::string, KeySlots> m_byKey;
    std::unordered_map<std::string, std::size_t> m_outsideByPath;
    /// The issues about single entries met, per code (EntryIssue).
    std::array<EntryIssueCount, kEntryIssueKinds> m_entryIssues{};
    /// The counted issues, per kind (Counted) in the order they were made.
    std::array<std::vector<Tally>, kCountedKinds> m_tallies;
};

} // namespace

fs::path gamelistPath(const fs::path& esdeHome, const catalog::SystemName& system) {
    // A SystemName is well-formed UTF-8 (SystemName::parse() checks it), which pathFromUtf8() never throws on.
    return esdeHome / "gamelists" / core::pathFromUtf8(system.str()) / "gamelist.xml";
}

Expected<Gamelist> readGamelist(const fs::path& file, const catalog::SystemName& system, const fs::path& romFolder,
                                std::uintmax_t maxBytes) {
    const std::string source = core::pathToUtf8Lossy(file);
    if (!romFolder.is_absolute()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the ROM folder a gamelist's paths are resolved against is not absolute: " +
                             core::pathToUtf8Lossy(romFolder),
                         source);
    }
    try {
        // Read and parsed here rather than by detail::loadXmlFile(), which logs what it read leniently at warning level
        // with the file's path: that path is the user's (never logged above debug level), and the warnings reach the
        // caller as issues.
        auto bytes = core::readFileShared(file, maxBytes);
        if (!bytes) {
            return propagate(std::move(bytes.error()));
        }
        auto document = detail::parseXml(std::move(*bytes), source);
        if (!document) {
            return propagate(std::move(document.error()));
        }
        for (const std::string& warning : document->warnings()) {
            spdlog::debug("{}: {}", source, warning);
        }
        const pugi::xml_node gameList = document->topLevel("gameList");
        if (!gameList) {
            return makeError(ErrorCode::ParseError,
                             "no <gameList> element: not a gamelist, or one that ES-DE is writing", source);
        }
        Gamelist gamelist = GamelistReader(*document, system, romFolder).read(gameList);
        spdlog::debug("Gamelist {}: {} game(s), {} folder(s), {} issue(s), alternative emulator \"{}\"", source,
                      gamelist.count(GamelistEntryKind::Game), gamelist.count(GamelistEntryKind::Folder),
                      gamelist.issues.size(), gamelist.alternativeEmulator);
        return gamelist;
    } catch (const std::bad_alloc&) {
        // Within maxBytes a file holds a few million elements at most, whose tree and entries may still not fit.
        return makeError(ErrorCode::OutOfRange, "out of memory while reading the gamelist", source);
    }
}

const GamelistEntry* entryForListed(std::span<const GamelistEntry* const> sameKey, const catalog::LookupKey& listed,
                                    GamelistEntryKind kind) noexcept {
    const GamelistEntry* lastOfKind = nullptr;
    for (const GamelistEntry* entry : sameKey) {
        if (entry->kind != kind) {
            continue;
        }
        if (entry->key.relativePath() == listed.relativePath()) {
            return entry; // at most one per kind and relative path: the parser kept the last
        }
        lastOfKind = entry;
    }
    if (lastOfKind != nullptr) {
        return lastOfKind;
    }
    return sameKey.empty() ? nullptr : sameKey.back();
}

} // namespace rv::esde
