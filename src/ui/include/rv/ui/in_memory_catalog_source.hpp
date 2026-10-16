#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/ui/catalog_source.hpp>

namespace rv::ui {

/// A CatalogSource over entries held in memory, for tests and mock-ups (spike_ui's 100,000 generated games):
///  - the list keeps the order of the entries given; entries with an invalid id (isValidEntryId) or the id of an
///    earlier entry are left out (rejected());
///  - platforms: one per distinct CatalogEntry::platform (the key and the label are its display name) in the order the
///    entries first name it, with the entries it holds; entries without a platform pass the filter of every game only;
///  - search: the text split into words at ASCII white space, every word must occur in the title, platform, developer,
///    publisher, genre or year of an entry, ignoring the case of ASCII letters (the catalog's FTS5 index also folds
///    diacritics, ADR-0008 §5). A query that only adds to the words of the previous one (typing on) searches the
///    previous list instead of the whole catalog;
///  - position() looks ids up in an index sorted by id, and the position of each entry in the list is kept: nothing is
///    scanned per call.
/// Memory per entry: the entry, its search text and four 32-bit indices. Main thread only.
class InMemoryCatalogSource final : public CatalogSource {
public:
    explicit InMemoryCatalogSource(std::vector<CatalogEntry> entries = {});

    /// Replaces the entries, as an import changes the catalog, and materializes the last query again (one naming a
    /// platform the new entries no longer have lists nothing).
    void replaceEntries(std::vector<CatalogEntry> entries);

    /// The entries kept, in list order of the blank query.
    [[nodiscard]] std::span<const CatalogEntry> entries() const noexcept { return m_entries; }
    [[nodiscard]] std::size_t rejected() const noexcept { return m_rejected; }
    /// The query the list was materialized from.
    [[nodiscard]] const CatalogSourceQuery& lastQuery() const noexcept { return m_query; }

    [[nodiscard]] Expected<std::vector<PlatformFilter>> platforms() override;
    [[nodiscard]] Status query(const CatalogSourceQuery& query) override;
    [[nodiscard]] std::size_t count() const noexcept override { return m_list.size(); }
    [[nodiscard]] Status rows(std::size_t first, std::size_t count, std::vector<CatalogEntry>& out) override;
    [[nodiscard]] std::optional<std::size_t> position(std::string_view id) const override;

private:
    /// Keeps the valid entries and builds the indices: by id, the platforms, the search texts.
    void index(std::vector<CatalogEntry> entries);
    [[nodiscard]] bool matches(std::uint32_t entry, std::span<const std::string> words) const;

    std::vector<CatalogEntry> m_entries;
    std::size_t m_rejected = 0;
    /// Entry indices ordered by id.
    std::vector<std::uint32_t> m_byId;
    std::vector<PlatformFilter> m_platforms;
    /// Per entry: the index of its platform in m_platforms (kNoPlatform without one), and its search text: the title,
    /// platform, developer, publisher, genre and year in lowercase ASCII, one per line.
    std::vector<std::uint32_t> m_platformOf;
    std::vector<std::string> m_searchText;

    CatalogSourceQuery m_query;
    /// m_query's words in lowercase, its platform's index (kNoPlatform: every platform).
    std::vector<std::string> m_words;
    std::uint32_t m_platform = 0;
    /// The materialized list (entry indices in order), and per entry its position in it (kNotListed when absent).
    std::vector<std::uint32_t> m_list;
    std::vector<std::uint32_t> m_positionOf;
    /// m_list is m_query's over the current entries (a later query that refines it may search it alone).
    bool m_listValid = false;
};

} // namespace rv::ui
