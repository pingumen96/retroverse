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
///    scanned per call;
///  - replaceEntries() stands for an import: the list stays the one materialized until the next query, its rows read
///    from the new entries by id (a row with its id alone where the entry is gone), as a source over the database does;
///  - setPendingPolls() makes queries answer later, as on the catalog worker.
/// Memory per entry: the entry, its search text and four 32-bit indices. Main thread only.
class InMemoryCatalogSource final : public CatalogSource {
public:
    explicit InMemoryCatalogSource(std::vector<CatalogEntry> entries = {});

    /// Replaces the entries, as an import changes the catalog. The list stays the one the last query materialized, as
    /// the contract says (CatalogSource::rows): its rows are read from the new entries by id, an id without an entry
    /// gives a row with the id alone. The next query lists the new entries.
    void replaceEntries(std::vector<CatalogEntry> entries);

    /// From now on query() answers Pending, and its list replaces the previous one at the `polls`-th poll() after it
    /// (a source whose queries run on the catalog worker); 0, the default, materializes within query().
    void setPendingPolls(std::uint32_t polls) noexcept { m_pendingPolls = polls; }

    /// The entries kept, in list order of the blank query.
    [[nodiscard]] std::span<const CatalogEntry> entries() const noexcept { return m_catalog.entries; }
    [[nodiscard]] std::size_t rejected() const noexcept { return m_catalog.rejected; }
    /// The query the list was materialized from.
    [[nodiscard]] const CatalogSourceQuery& lastQuery() const noexcept { return m_query; }
    /// A query waits for poll().
    [[nodiscard]] bool pending() const noexcept { return m_pending.has_value(); }

    [[nodiscard]] Expected<std::vector<PlatformFilter>> platforms() override;
    [[nodiscard]] Expected<QueryState> query(const CatalogSourceQuery& query) override;
    [[nodiscard]] Expected<QueryState> poll() override;
    [[nodiscard]] std::size_t count() const noexcept override { return m_list.size(); }
    [[nodiscard]] Status rows(std::size_t first, std::size_t count, std::vector<CatalogEntry>& out) override;
    [[nodiscard]] std::optional<std::size_t> position(std::string_view id) const override;

private:
    /// Entries and their indices: by id, the platforms, the search texts.
    struct Catalog {
        std::vector<CatalogEntry> entries;
        std::size_t rejected = 0;
        /// Entry indices ordered by id.
        std::vector<std::uint32_t> byId;
        std::vector<PlatformFilter> platforms;
        /// Per entry: the index of its platform in `platforms` (none: the maximum), and its search text: the title,
        /// platform, developer, publisher, genre and year in lowercase ASCII, one per line.
        std::vector<std::uint32_t> platformOf;
        std::vector<std::string> searchText;

        /// The index of the entry `id`, if any.
        [[nodiscard]] std::optional<std::uint32_t> find(std::string_view id) const;
    };

    /// Keeps the valid entries and builds the indices.
    [[nodiscard]] static Catalog index(std::vector<CatalogEntry> entries);
    /// Materializes `query` over the current entries.
    [[nodiscard]] Status materialize(const CatalogSourceQuery& query);
    [[nodiscard]] bool matches(std::uint32_t entry, std::span<const std::string> words) const;
    /// The entries the list indexes: those of the last query's time (the current ones unless replaced since).
    [[nodiscard]] const Catalog& listed() const noexcept { return m_listed ? *m_listed : m_catalog; }

    Catalog m_catalog;
    /// The entries the list was materialized from, while replaceEntries() has replaced them since.
    std::optional<Catalog> m_listed;

    CatalogSourceQuery m_query;
    /// m_query's words in lowercase, its platform's index (none: every platform).
    std::vector<std::string> m_words;
    std::uint32_t m_platform = 0;
    /// The materialized list (indices into listed().entries in order), and per entry of listed() its position in it
    /// (none when absent).
    std::vector<std::uint32_t> m_list;
    std::vector<std::uint32_t> m_positionOf;
    /// m_list is m_query's over the current entries (a later query that refines it may search it alone).
    bool m_listValid = false;

    /// setPendingPolls(), the query waiting for poll() and the polls before it completes.
    std::uint32_t m_pendingPolls = 0;
    std::optional<CatalogSourceQuery> m_pending;
    std::uint32_t m_pollsLeft = 0;
};

} // namespace rv::ui
