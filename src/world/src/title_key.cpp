#include <rv/world/title_key.hpp>

#include <algorithm>
#include <array>
#include <tuple>
#include <vector>

#include <rv/catalog/metadata.hpp>
#include <rv/core/text.hpp>

namespace rv::world {
namespace {

/// ASCII white space, as catalog::sortKeyOf and core::trim read it.
constexpr std::string_view kWhitespace = " \t\r\n\f\v";

bool isWhitespace(char c) noexcept {
    return kWhitespace.find(c) != std::string_view::npos;
}

/// A group of a text: what lies between an opening character and the one that balances it.
struct Group {
    char open = '(';
    std::string_view content;
    std::size_t begin = 0; ///< the opening character
    std::size_t end = 0;   ///< one past the closing character
};

/// The groups of `text`, from the left (the header's rule 1): an opening '(' or '[' not inside an earlier group starts
/// one, which ends at the closing character that balances it, only characters of its own kind counted. An opening
/// character nothing balances is text. UTF-8 is scanned byte by byte: no byte of a multi-byte character is ASCII.
std::vector<Group> groupsOf(std::string_view text) {
    std::vector<Group> groups;
    std::size_t i = 0;
    while (i < text.size()) {
        const char open = text[i];
        if (open != '(' && open != '[') {
            ++i;
            continue;
        }
        const char close = open == '(' ? ')' : ']';
        std::size_t depth = 0;
        std::size_t j = i;
        for (; j < text.size(); ++j) {
            if (text[j] == open) {
                ++depth;
            } else if (text[j] == close && --depth == 0) {
                break;
            }
        }
        if (j == text.size()) {
            ++i; // unbalanced: the character stays as text, and what follows it is scanned
            continue;
        }
        groups.push_back(Group{open, text.substr(i + 1, j - i - 1), i, j + 1});
        i = j + 1;
    }
    return groups;
}

/// The items of a group's content, ", " between them.
std::vector<std::string_view> itemsOf(std::string_view content) {
    std::vector<std::string_view> items;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = content.find(", ", start);
        items.push_back(
            content.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (comma == std::string_view::npos) {
            return items;
        }
        start = comma + 2;
    }
}

/// No-Intro's region names (its naming convention and the regions its DAT files use), as written.
constexpr std::array<std::string_view, 45> kNoIntroRegions{
    "Argentina",      "Asia",    "Australia", "Austria",     "Belgium",       "Brazil",      "Canada",
    "Chile",          "China",   "Croatia",   "Czech",       "Denmark",       "Europe",      "Finland",
    "France",         "Germany", "Greece",    "Hong Kong",   "Hungary",       "India",       "Ireland",
    "Israel",         "Italy",   "Japan",     "Korea",       "Latin America", "Mexico",      "Netherlands",
    "New Zealand",    "Norway",  "Poland",    "Portugal",    "Russia",        "Scandinavia", "Slovakia",
    "South Africa",   "Spain",   "Sweden",    "Switzerland", "Taiwan",        "Turkey",      "UK",
    "United Kingdom", "USA",     "World",
};

/// GoodTools' country codes written alone (GoodCodes' list; PD, Unl and Unk name no country): England (UK) and the
/// two-letter codes are matched whole before a code is read as single letters combined.
constexpr std::array<std::string_view, 21> kGoodToolsCodes{
    "1", "4", "A", "B", "C", "E", "F", "FC", "FN", "G", "GR", "HK", "I", "J", "K", "NL", "S", "SW", "U", "UK", "W",
};

/// The single-letter codes GoodTools combines ("JU", "UE", "JUE").
constexpr std::string_view kGoodToolsLetters = "ABCEFGIJKSUW";

/// The rank of a region tag's content, nothing when the group is no region tag.
std::optional<RegionRank> regionRankOf(std::string_view content) {
    if (content.empty()) {
        return std::nullopt;
    }
    const auto rankOf = [](bool usaOrWorld, bool europe) {
        return usaOrWorld ? RegionRank::UsaOrWorld : europe ? RegionRank::Europe : RegionRank::Other;
    };
    // GoodTools: a code alone, then single letters combined (each at most once).
    if (std::find(kGoodToolsCodes.begin(), kGoodToolsCodes.end(), content) != kGoodToolsCodes.end()) {
        return rankOf(content == "U" || content == "W" || content == "4", content == "E");
    }
    if (content.size() >= 2 && std::all_of(content.begin(), content.end(), [&](char c) {
            return kGoodToolsLetters.find(c) != std::string_view::npos &&
                   std::count(content.begin(), content.end(), c) == 1;
        })) {
        return rankOf(content.find_first_of("UW") != std::string_view::npos,
                      content.find('E') != std::string_view::npos);
    }
    // No-Intro: region names, ", " between them.
    bool usaOrWorld = false;
    bool europe = false;
    for (const std::string_view item : itemsOf(content)) {
        if (std::find(kNoIntroRegions.begin(), kNoIntroRegions.end(), item) == kNoIntroRegions.end()) {
            return std::nullopt;
        }
        usaOrWorld = usaOrWorld || item == "USA" || item == "World";
        europe = europe || item == "Europe";
    }
    return rankOf(usaOrWorld, europe);
}

/// No-Intro's status words, the first word of an item.
constexpr std::array<std::string_view, 5> kStatusWords{"Beta", "Proto", "Demo", "Sample", "Pirate"};

bool isStatusGroup(const Group& group) {
    if (group.open == '[') {
        // GoodTools' marks: [h...], [b...], [a...], the letter not followed by another lower-case letter.
        const std::string_view mark = group.content;
        return !mark.empty() && (mark[0] == 'h' || mark[0] == 'b' || mark[0] == 'a') &&
               (mark.size() == 1 || mark[1] < 'a' || mark[1] > 'z');
    }
    for (const std::string_view item : itemsOf(group.content)) {
        const std::string_view word = item.substr(0, item.find(' '));
        if (std::find(kStatusWords.begin(), kStatusWords.end(), word) != kStatusWords.end()) {
            return true;
        }
    }
    return false;
}

/// N of an item "Rev N" (1 to 9 decimal digits), nothing for any other item.
std::optional<std::uint32_t> revisionOf(std::string_view item) {
    constexpr std::string_view kPrefix = "Rev ";
    if (!item.starts_with(kPrefix)) {
        return std::nullopt;
    }
    const std::string_view digits = item.substr(kPrefix.size());
    if (digits.empty() || digits.size() > 9 ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : digits) {
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

/// `text` with its white space trimmed and every run inside made one space.
std::string collapsedWhitespace(std::string_view text) {
    const std::string_view trimmed = core::trim(text);
    std::string collapsed;
    collapsed.reserve(trimmed.size());
    for (std::size_t i = 0; i < trimmed.size(); ++i) {
        if (!isWhitespace(trimmed[i])) {
            collapsed += trimmed[i];
        } else if (!isWhitespace(trimmed[i - 1])) { // i > 0: the text does not start with white space
            collapsed += ' ';
        }
    }
    return collapsed;
}

/// The subtitle separators made one (the header's rule 3), from the left.
std::string unifiedSeparators(std::string_view text) {
    static constexpr std::array<std::string_view, 3> kSeparators{" : ", " - ", " \xE2\x80\x93 "};
    std::string unified;
    unified.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto separator = std::find_if(kSeparators.begin(), kSeparators.end(),
                                            [&](std::string_view s) { return text.substr(i).starts_with(s); });
        if (separator != kSeparators.end()) {
            unified += ": ";
            i += separator->size();
        } else {
            unified += text[i++];
        }
    }
    return unified;
}

} // namespace

Expected<std::string> titleKey(std::string_view title) {
    std::string stripped;
    stripped.reserve(title.size());
    std::size_t next = 0;
    for (const Group& group : groupsOf(title)) {
        stripped.append(title.substr(next, group.begin - next));
        next = group.end;
    }
    stripped.append(title.substr(next));
    const std::string key = unifiedSeparators(collapsedWhitespace(stripped));
    return catalog::sortKeyOf(key.empty() ? title : std::string_view(key));
}

VersionTags readVersionTags(std::string_view fileName) {
    VersionTags tags;
    for (const Group& group : groupsOf(fileName)) {
        if (group.open == '(' && !tags.regionTag) {
            if (const auto rank = regionRankOf(group.content)) {
                tags.region = *rank;
                tags.regionTag = std::string(group.content);
                continue;
            }
        }
        tags.status = tags.status || isStatusGroup(group);
        if (group.open == '(') {
            for (const std::string_view item : itemsOf(group.content)) {
                if (const auto revision = revisionOf(item)) {
                    tags.revision = std::max(tags.revision, *revision);
                }
            }
        }
    }
    return tags;
}

std::string_view fileNameOf(std::string_view relativePath) noexcept {
    const std::size_t slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1);
}

bool versionTagsLess(const VersionTags& a, const VersionTags& b) noexcept {
    // The highest revision first: compared the other way round.
    return std::tuple(a.region, a.status, b.revision) < std::tuple(b.region, b.status, a.revision);
}

} // namespace rv::world
