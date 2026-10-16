#pragma once

// The query and paging contract of the catalog (ADR-0008 §5). A query (filters, search text, sort order) is
// materialized in one read as the ordered list of the GameIds it matches: the list's size is the count, a page is a
// slice of it whose rows are fetched by id (ICatalogQuery::fetch), and the position of an id is its index in the list,
// so a virtualized view can jump anywhere. The order is total: each sort order ends with (sort key, GameId). A list is
// materialized again when an import completes, and the view restores its selection by GameId.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/metadata.hpp>

namespace rv::catalog {

/// The order of a materialized list. Each one is total: it ends with the entries' sort keys (kSortKeyVersion), then
/// their GameIds, ascending.
enum class SortOrder : std::uint8_t {
    Title,       ///< (sort key, GameId)
    System,      ///< the system's full name folded as sort keys are, its name, then Title's order
    ReleaseDate, ///< the release date, oldest first and undated entries last, then Title's order
};

/// "title", "system", "release-date".
[[nodiscard]] constexpr std::string_view toString(SortOrder order) noexcept {
    switch (order) {
    case SortOrder::Title:
        return "title";
    case SortOrder::System:
        return "system";
    case SortOrder::ReleaseDate:
        return "release-date";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<SortOrder> parseSortOrder(std::string_view text) noexcept {
    return detail::enumFromString(text, SortOrder::ReleaseDate);
}

/// What a list shows. An entry matches when it passes every filter.
struct CatalogQuery {
    /// What the user typed (ADR-0008 §5): split into words as the search index splits text, every word must match the
    /// title, sort title, developer, publisher, genre, year or the system's full name, the last one as a prefix;
    /// diacritics and case ignored. Blank text matches every entry.
    std::string text;
    /// The systems whose entries match; empty: every system.
    std::vector<SystemName> systems;
    /// The entry states that match; empty: Game and MissingFile (the games as the catalog shows them).
    /// OutsideSystemFolder matches nothing: such entries are never indexed.
    std::vector<EntryState> states;
    /// The availabilities that match; empty: every one includeUnavailable lets through.
    std::vector<Availability> availabilities;
    /// Also entries that are not Available: missing files and entries under a folder that could not be listed. Off by
    /// default (FR-033: unavailable games are hidden unless asked for).
    bool includeUnavailable = false;
    SortOrder sort = SortOrder::Title;

    bool operator==(const CatalogQuery&) const = default;
};

/// An entry as lists and cards show it.
struct CatalogRow {
    GameId id;
    SystemName system;
    /// The system's <fullname>.
    std::string systemFullName;
    /// CatalogEntry::title.
    std::string title;
    EntryState state = EntryState::Game;
    Availability availability = Availability::Available;
    std::optional<ReleaseDate> releaseDate;
    /// displayText() of the metadata's text fields.
    std::string developer;
    std::string publisher;
    std::string genre;
    std::optional<Players> players;
    std::optional<Rating> rating;
    /// The <desc> as written (line feeds kept).
    std::string description;
    bool favorite = false;

    bool operator==(const CatalogRow&) const = default;
};

/// The ids at positions [offset, offset + count) of a materialized list, cut at its end (empty past it).
[[nodiscard]] inline std::span<const GameId> pageOf(std::span<const GameId> list, std::size_t offset,
                                                    std::size_t count) noexcept {
    if (offset >= list.size()) {
        return {};
    }
    return list.subspan(offset, std::min(count, list.size() - offset));
}

/// The position of `id` in a materialized list, or nothing when the list does not hold it. Linear: a view that asks
/// often keeps its own index of the list.
[[nodiscard]] inline std::optional<std::size_t> positionOf(std::span<const GameId> list, GameId id) noexcept {
    const auto found = std::find(list.begin(), list.end(), id);
    if (found == list.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - list.begin());
}

} // namespace rv::catalog
