#include <rv/ui/in_memory_catalog_source.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <utility>

namespace rv::ui {
namespace {

constexpr std::uint32_t kNoPlatform = std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t kNotListed = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] char lowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool isAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// The words of `text` in lowercase ASCII, split at ASCII white space.
[[nodiscard]] std::vector<std::string> wordsOf(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : text) {
        if (isAsciiSpace(c)) {
            if (!word.empty()) {
                words.push_back(std::move(word));
                word.clear();
            }
            continue;
        }
        word.push_back(lowerAscii(c));
    }
    if (!word.empty()) {
        words.push_back(std::move(word));
    }
    return words;
}

/// Appends `field` in lowercase ASCII and a line break: the line break keeps a word from matching across two fields
/// (words hold no white space).
void appendField(std::string& text, std::string_view field) {
    for (const char c : field) {
        text.push_back(lowerAscii(c));
    }
    text.push_back('\n');
}

/// True when every word of `previous` occurs in a word of `next`: an entry that matches `next` then matches `previous`
/// too, so `next` only needs to search the list `previous` made.
[[nodiscard]] bool refines(std::span<const std::string> next, std::span<const std::string> previous) {
    return std::all_of(previous.begin(), previous.end(), [next](const std::string& earlier) {
        return std::any_of(next.begin(), next.end(),
                           [&earlier](const std::string& word) { return word.find(earlier) != std::string::npos; });
    });
}

} // namespace

InMemoryCatalogSource::InMemoryCatalogSource(std::vector<CatalogEntry> entries) {
    index(std::move(entries));
    static_cast<void>(InMemoryCatalogSource::query(CatalogSourceQuery{}));
}

void InMemoryCatalogSource::replaceEntries(std::vector<CatalogEntry> entries) {
    index(std::move(entries));
    const CatalogSourceQuery last = m_query;
    static_cast<void>(InMemoryCatalogSource::query(last));
}

void InMemoryCatalogSource::index(std::vector<CatalogEntry> entries) {
    m_rejected = 0;
    // Ids first: invalid ones out, then the later entries of an id that repeats (a stable sort by id keeps the entries
    // of one id in their order, so the first of a run is the earliest).
    std::vector<std::uint32_t> byId;
    byId.reserve(entries.size());
    for (std::size_t entry = 0; entry < entries.size(); ++entry) {
        if (isValidEntryId(entries[entry].id)) {
            byId.push_back(static_cast<std::uint32_t>(entry));
        } else {
            ++m_rejected;
        }
    }
    std::stable_sort(byId.begin(), byId.end(),
                     [&entries](std::uint32_t a, std::uint32_t b) { return entries[a].id < entries[b].id; });
    std::vector<bool> kept(entries.size(), false);
    for (std::size_t at = 0; at < byId.size(); ++at) {
        const bool repeated = at > 0 && entries[byId[at]].id == entries[byId[at - 1]].id;
        kept[byId[at]] = !repeated;
        m_rejected += repeated ? 1 : 0;
    }
    // The kept entries in their order, and the id index renumbered to them.
    std::vector<std::uint32_t> renumbered(entries.size(), kNotListed);
    m_entries.clear();
    m_entries.reserve(entries.size() - m_rejected);
    for (std::size_t entry = 0; entry < entries.size(); ++entry) {
        if (kept[entry]) {
            renumbered[entry] = static_cast<std::uint32_t>(m_entries.size());
            m_entries.push_back(std::move(entries[entry]));
        }
    }
    m_byId.clear();
    m_byId.reserve(m_entries.size());
    for (const std::uint32_t entry : byId) {
        if (kept[entry]) {
            m_byId.push_back(renumbered[entry]);
        }
    }

    // Platforms in the order the entries first name them, and the search texts.
    m_platforms.clear();
    m_platformOf.assign(m_entries.size(), kNoPlatform);
    m_searchText.assign(m_entries.size(), std::string());
    std::map<std::string, std::uint32_t, std::less<>> platformIndex;
    for (std::size_t entry = 0; entry < m_entries.size(); ++entry) {
        const CatalogEntry& item = m_entries[entry];
        if (!item.platform.empty()) {
            auto found = platformIndex.find(item.platform);
            if (found == platformIndex.end()) {
                found = platformIndex.emplace(item.platform, static_cast<std::uint32_t>(m_platforms.size())).first;
                m_platforms.push_back(PlatformFilter{item.platform, item.platform, 0});
            }
            m_platformOf[entry] = found->second;
            ++m_platforms[found->second].count;
        }
        std::string& text = m_searchText[entry];
        text.reserve(item.title.size() + item.platform.size() + item.developer.size() + item.publisher.size() +
                     item.genre.size() + 12);
        appendField(text, item.title);
        appendField(text, item.platform);
        appendField(text, item.developer);
        appendField(text, item.publisher);
        appendField(text, item.genre);
        appendField(text, item.year > 0 ? std::to_string(item.year) : std::string());
    }
    m_list.clear();
    m_words.clear();
    m_listValid = false;
    m_positionOf.assign(m_entries.size(), kNotListed);
}

Expected<std::vector<PlatformFilter>> InMemoryCatalogSource::platforms() {
    return m_platforms;
}

Status InMemoryCatalogSource::query(const CatalogSourceQuery& query) {
    std::uint32_t platform = kNoPlatform;
    if (!query.platform.empty()) {
        const auto found = std::find_if(m_platforms.begin(), m_platforms.end(), [&query](const PlatformFilter& filter) {
            return filter.key == query.platform;
        });
        platform = found != m_platforms.end() ? static_cast<std::uint32_t>(found - m_platforms.begin()) : kNoPlatform;
    }
    const bool unknownPlatform = !query.platform.empty() && platform == kNoPlatform;
    std::vector<std::string> words = wordsOf(query.text);

    // Typing on narrows the list: an entry the new words match was in the previous list (the same platform).
    const bool narrower = m_listValid && platform == m_platform && refines(words, m_words);
    std::vector<std::uint32_t> list;
    if (!unknownPlatform) {
        const auto consider = [&](std::uint32_t entry) {
            if ((platform == kNoPlatform || m_platformOf[entry] == platform) && matches(entry, words)) {
                list.push_back(entry);
            }
        };
        if (narrower) {
            list.reserve(m_list.size());
            for (const std::uint32_t entry : m_list) {
                consider(entry);
            }
        } else {
            list.reserve(platform == kNoPlatform ? m_entries.size() : m_platforms[platform].count);
            for (std::uint32_t entry = 0; entry < m_entries.size(); ++entry) {
                consider(entry);
            }
        }
    }
    for (const std::uint32_t entry : m_list) {
        m_positionOf[entry] = kNotListed;
    }
    m_list = std::move(list);
    for (std::size_t at = 0; at < m_list.size(); ++at) {
        m_positionOf[m_list[at]] = static_cast<std::uint32_t>(at);
    }
    m_query = query;
    m_words = std::move(words);
    m_platform = platform;
    m_listValid = !unknownPlatform;
    if (unknownPlatform) {
        return makeError(ErrorCode::InvalidArgument, "the catalog has no such platform", query.platform);
    }
    return ok();
}

bool InMemoryCatalogSource::matches(std::uint32_t entry, std::span<const std::string> words) const {
    const std::string& text = m_searchText[entry];
    return std::all_of(words.begin(), words.end(),
                       [&text](const std::string& word) { return text.find(word) != std::string::npos; });
}

Status InMemoryCatalogSource::rows(std::size_t first, std::size_t count, std::vector<CatalogEntry>& out) {
    if (first >= m_list.size()) {
        out.clear();
        return ok();
    }
    const std::size_t rows = std::min(count, m_list.size() - first);
    out.resize(rows);
    for (std::size_t row = 0; row < rows; ++row) {
        out[row] = m_entries[m_list[first + row]];
    }
    return ok();
}

std::optional<std::size_t> InMemoryCatalogSource::position(std::string_view id) const {
    const auto found =
        std::lower_bound(m_byId.begin(), m_byId.end(), id, [this](std::uint32_t entry, std::string_view key) {
            return std::string_view(m_entries[entry].id) < key;
        });
    if (found == m_byId.end() || m_entries[*found].id != id) {
        return std::nullopt;
    }
    const std::uint32_t at = m_positionOf[*found];
    if (at == kNotListed) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(at);
}

} // namespace rv::ui
