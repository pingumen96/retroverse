#include <rv/ui/quick_catalog_model.hpp>

#include <algorithm>
#include <string>
#include <utility>

#include <rv/core/text.hpp>

namespace rv::ui {
namespace {

constexpr std::size_t kNoSelection = static_cast<std::size_t>(-1);
constexpr std::size_t kMaxEntryIdLength = 64;

/// True when `text` contains `lowercaseWord`, ignoring the case of ASCII letters.
[[nodiscard]] bool containsWord(std::string_view text, std::string_view lowercaseWord) {
    if (lowercaseWord.empty()) {
        return true;
    }
    const auto lower = [](char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    const auto found = std::search(text.begin(), text.end(), lowercaseWord.begin(), lowercaseWord.end(),
                                   [&lower](char a, char b) { return lower(a) == b; });
    return found != text.end();
}

} // namespace

bool isValidEntryId(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxEntryIdLength) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

QuickCatalogModel::QuickCatalogModel(std::vector<CatalogEntry> entries, std::string allLabel) {
    m_entries.reserve(entries.size());
    for (CatalogEntry& entry : entries) {
        const bool duplicate = std::any_of(m_entries.begin(), m_entries.end(),
                                           [&entry](const CatalogEntry& kept) { return kept.id == entry.id; });
        if (!isValidEntryId(entry.id) || duplicate) {
            ++m_rejected;
            continue;
        }
        m_entries.push_back(std::move(entry));
    }
    m_filters.push_back(PlatformFilter{{}, std::move(allLabel), m_entries.size()});
    for (const CatalogEntry& entry : m_entries) {
        const auto known =
            std::find_if(m_filters.begin() + 1, m_filters.end(),
                         [&entry](const PlatformFilter& filter) { return filter.key == entry.platform; });
        if (known != m_filters.end()) {
            ++known->count;
        } else if (!entry.platform.empty()) {
            m_filters.push_back(PlatformFilter{entry.platform, entry.platform, 1});
        }
    }
    m_visible.reserve(m_entries.size());
    refresh();
}

const CatalogEntry* QuickCatalogModel::find(std::string_view id) const noexcept {
    const auto found =
        std::find_if(m_entries.begin(), m_entries.end(), [id](const CatalogEntry& entry) { return entry.id == id; });
    return found != m_entries.end() ? &*found : nullptr;
}

bool QuickCatalogModel::setFilter(std::string_view key) {
    const bool known = std::any_of(m_filters.begin(), m_filters.end(),
                                   [key](const PlatformFilter& filter) { return filter.key == key; });
    if (!known) {
        return false;
    }
    if (m_activeFilter != key) {
        m_activeFilter = std::string(key);
        ++m_filterRevision;
        refresh();
    }
    return true;
}

void QuickCatalogModel::setSearch(std::string_view text) {
    if (m_search == text) {
        return;
    }
    m_search = std::string(text);
    m_searchWords.clear();
    for (const std::string_view word : core::split(core::trim(text), ' ')) {
        if (const std::string_view trimmed = core::trim(word); !trimmed.empty()) {
            m_searchWords.push_back(core::toLowerAscii(trimmed));
        }
    }
    refresh();
}

bool QuickCatalogModel::passes(const CatalogEntry& entry) const {
    if (!m_activeFilter.empty() && entry.platform != m_activeFilter) {
        return false;
    }
    if (m_searchWords.empty()) {
        return true;
    }
    const std::string year = entry.year > 0 ? std::to_string(entry.year) : std::string();
    return std::all_of(m_searchWords.begin(), m_searchWords.end(), [&](const std::string& word) {
        return containsWord(entry.title, word) || containsWord(entry.platform, word) ||
               containsWord(entry.developer, word) || containsWord(entry.publisher, word) ||
               containsWord(entry.genre, word) || containsWord(year, word);
    });
}

void QuickCatalogModel::refresh() {
    std::vector<std::size_t> visible;
    visible.reserve(m_entries.size());
    for (std::size_t index = 0; index < m_entries.size(); ++index) {
        if (passes(m_entries[index])) {
            visible.push_back(index);
        }
    }
    if (visible != m_visible || m_visibleRevision == 0) {
        m_visible = std::move(visible);
        ++m_visibleRevision;
    }
    const bool selectionVisible =
        m_selected != kNoSelection && std::find(m_visible.begin(), m_visible.end(), m_selected) != m_visible.end();
    if (!selectionVisible) {
        const std::size_t next = m_visible.empty() ? kNoSelection : m_visible.front();
        if (next != m_selected) {
            m_selected = next;
            ++m_selectionRevision;
        }
    }
}

bool QuickCatalogModel::isVisible(std::string_view id) const noexcept {
    return std::any_of(m_visible.begin(), m_visible.end(),
                       [this, id](std::size_t index) { return m_entries[index].id == id; });
}

bool QuickCatalogModel::select(std::string_view id) {
    const auto found = std::find_if(m_visible.begin(), m_visible.end(),
                                    [this, id](std::size_t index) { return m_entries[index].id == id; });
    if (found == m_visible.end()) {
        return false;
    }
    if (*found != m_selected) {
        m_selected = *found;
        ++m_selectionRevision;
    }
    return true;
}

const CatalogEntry* QuickCatalogModel::selected() const noexcept {
    return m_selected != kNoSelection ? &m_entries[m_selected] : nullptr;
}

} // namespace rv::ui
