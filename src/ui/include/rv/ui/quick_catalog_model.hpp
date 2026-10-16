#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rv::ui {

/// A game as the quick catalog shows it (specification §13.2: title, platform, year, developer, publisher, genre).
/// Catalog data, shown as it comes (style §17.2).
struct CatalogEntry {
    /// Stable and unique; also names the game's element in documents ("game-<id>"), so it holds only ASCII letters,
    /// digits, '-' and '_' (isValidEntryId).
    std::string id;
    std::string title;
    /// Display name of its platform: one filter chip per platform.
    std::string platform;
    /// Year of release, 0 when unknown.
    int year = 0;
    std::string developer;
    std::string publisher;
    std::string genre;
    /// Number of players as the catalog writes it ("1", "1-2").
    std::string players;
    std::string description;
};

/// True when `id` is a usable CatalogEntry::id: 1 to 64 ASCII letters, digits, '-' or '_'.
[[nodiscard]] bool isValidEntryId(std::string_view id) noexcept;

/// A filter chip: every game (empty key) or one platform.
struct PlatformFilter {
    std::string key;   ///< empty for every game, else the platform's display name
    std::string label; ///< what the chip shows
    std::size_t count = 0; ///< games of the catalog it lets through, the search aside
};

/// The view model of the quick catalog (specification §12.2 QuickLibrary, §13.2): the catalog's games, the platform
/// filter, the search text, which games pass both, and the selected game whose card is shown. GPU-free, toolkit-free:
/// the RmlUi screen binds it (rv::ui::QuickCatalogScreen) and a Dear ImGui fallback could bind the same model.
///
///  - Filters: every game first ("Tutte"), then one per platform in the order the catalog first names it.
///  - Search: case-insensitive (ASCII letters) words, each of which must occur in the title, platform, developer,
///    publisher, genre or year of a game; surrounding spaces are ignored, so an empty or blank search passes every
///    game.
///  - Selection: a visible game, or none when nothing is visible. It survives filter and search changes that keep it
///    visible, else it moves to the first visible game.
/// The revisions count the changes of visible() and of the selection, so that a view refreshes only what changed.
class QuickCatalogModel {
public:
    /// Keeps the entries in order; entries with an invalid or duplicate id are left out (rejected() counts them).
    explicit QuickCatalogModel(std::vector<CatalogEntry> entries, std::string allLabel = "Tutte");

    [[nodiscard]] std::span<const CatalogEntry> entries() const noexcept { return m_entries; }
    [[nodiscard]] std::size_t rejected() const noexcept { return m_rejected; }
    [[nodiscard]] const CatalogEntry* find(std::string_view id) const noexcept;

    [[nodiscard]] std::span<const PlatformFilter> filters() const noexcept { return m_filters; }
    /// The key of the active filter (empty: every game).
    [[nodiscard]] const std::string& activeFilter() const noexcept { return m_activeFilter; }
    /// Activates the filter with `key`; false, and nothing changes, for a key no filter has.
    bool setFilter(std::string_view key);

    [[nodiscard]] const std::string& search() const noexcept { return m_search; }
    void setSearch(std::string_view text);

    /// Indices into entries() of the games that pass the filter and the search, in catalog order.
    [[nodiscard]] std::span<const std::size_t> visible() const noexcept { return m_visible; }
    [[nodiscard]] bool isVisible(std::string_view id) const noexcept;

    /// Selects a visible game; false, and nothing changes, for a game that is not visible.
    bool select(std::string_view id);
    /// The selected game, or nothing when no game is visible.
    [[nodiscard]] const CatalogEntry* selected() const noexcept;

    /// Incremented whenever visible() changes.
    [[nodiscard]] std::uint64_t visibleRevision() const noexcept { return m_visibleRevision; }
    /// Incremented whenever the selected game changes.
    [[nodiscard]] std::uint64_t selectionRevision() const noexcept { return m_selectionRevision; }

private:
    void refresh();
    [[nodiscard]] bool passes(const CatalogEntry& entry) const;

    std::vector<CatalogEntry> m_entries;
    std::size_t m_rejected = 0;
    std::vector<PlatformFilter> m_filters;
    std::string m_activeFilter;
    std::string m_search;
    /// The search split into lowercase words.
    std::vector<std::string> m_searchWords;
    std::vector<std::size_t> m_visible;
    /// Index into m_entries of the selected game; npos for none.
    std::size_t m_selected = static_cast<std::size_t>(-1);
    std::uint64_t m_visibleRevision = 0;
    std::uint64_t m_selectionRevision = 0;
};

} // namespace rv::ui
