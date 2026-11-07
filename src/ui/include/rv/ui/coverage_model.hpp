#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The media coverage screen (ADR-0018 §14; assets/ui/coverage.rml through ui::CoverageScreen): per system and media kind,
// the share of the games that have a file of the kind, worst first, over the catalog's own rows (two populations: every
// game, and the games a gamelist names), the clips known to fail (<user data>/cache/media/videos.json), and what to do:
// scrape in ES-DE (ADR-0003). The figures are the catalog worker's, in text as they show (app::coverageContentOf); every
// word is the document's. The model holds what shows and the focus.
//
// | View    | Up, Down, pages, Home, End | Left, Right, filters | Activate          | Back              |
// |---------|----------------------------|----------------------|-------------------|-------------------|
// | Systems | the focused system         | the population       | opens the system  | closes            |
// | System  | (the screen scrolls)       | the population       |                   | back to Systems   |
//
// Clicks press what the keys press (VR's ray too). Every change of what shows increments revision().

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace coverage {
inline constexpr std::string_view kDocumentPath = "ui/coverage.rml";
inline constexpr std::string_view kDataModel = "coverage";
inline constexpr std::string_view kPanelId = "coverage-panel";
/// The region a system's details scroll in.
inline constexpr std::string_view kDetailId = "cv-detail";
/// The rows Page Up and Page Down move.
inline constexpr std::size_t kPageRows = 8;
} // namespace coverage

enum class CoverageState : std::uint8_t {
    Loading,     ///< asked of the catalog
    Listed,      ///< its figures show
    Empty,       ///< the catalog has no game
    Unavailable, ///< no catalog is open, or it cannot be read
};
/// "loading", "listed", "empty", "unavailable".
[[nodiscard]] std::string_view toString(CoverageState state) noexcept;

enum class CoverageView : std::uint8_t { Systems, System };
/// "systems", "system".
[[nodiscard]] std::string_view toString(CoverageView view) noexcept;

/// Which games the shares count (catalog::CoveragePopulation's Games and ScrapedGames).
enum class CoverageTab : std::uint8_t { Games, Scraped };
inline constexpr std::array<CoverageTab, 2> kCoverageTabs = {CoverageTab::Games, CoverageTab::Scraped};
/// "games", "scraped" (the click event's argument).
[[nodiscard]] std::string_view toString(CoverageTab tab) noexcept;
[[nodiscard]] std::optional<CoverageTab> coverageTabFromName(std::string_view name) noexcept;

/// One kind's figures in one population.
struct CoverageShare {
    /// catalog::toString(MediaKind): "covers", "screenshots", ...
    std::string kind;
    /// As it shows: "90,9" (percent, one decimal, a comma), "-" when nothing is known.
    std::string share;
    std::uint64_t with = 0;
    std::uint64_t missing = 0;
    /// Under half of the games have it.
    bool low = false;

    bool operator==(const CoverageShare&) const = default;
};

/// A system's row (or the total) in one population.
struct CoverageRow {
    std::string name;
    std::string fullName;
    std::uint64_t games = 0;
    /// The kinds the list shows, as shares: covers, 3D boxes, screenshots, marquees, videos.
    std::string covers;
    std::string boxes;
    std::string screenshots;
    std::string marquees;
    std::string videos;
    /// Every kind, in the order of catalog::kMediaKinds (the system's details).
    std::vector<CoverageShare> kinds;
    /// The games with at least one kind.
    std::string any;
    /// The clips of the system known to fail.
    std::uint64_t clipFailures = 0;

    bool operator==(const CoverageRow&) const = default;
};

/// One population's table: the total, then the systems with at least one member, worst first.
struct CoverageTable {
    CoverageRow total;
    std::vector<CoverageRow> systems;

    bool operator==(const CoverageTable&) const = default;
};

/// A clip known to fail.
struct CoverageClip {
    std::string system;
    /// Its file name, below the system's videos folder.
    std::string file;
    /// media::verdictName(): "corrupt-data", "truncated", ...
    std::string verdict;

    bool operator==(const CoverageClip&) const = default;
};

/// What the screen shows.
struct CoverageContent {
    /// The catalog has no game.
    bool empty = true;
    CoverageTable games;
    CoverageTable scraped;
    /// The failure verdicts, by system then file.
    std::vector<CoverageClip> clips;
    /// The verdicts file could not be read: the clips' failures are not known.
    bool clipsUnknown = false;

    [[nodiscard]] const CoverageTable& table(CoverageTab tab) const noexcept {
        return tab == CoverageTab::Scraped ? scraped : games;
    }

    bool operator==(const CoverageContent&) const = default;
};

enum class CoverageCommand : std::uint8_t {
    Close, ///< it closed itself
    Load,  ///< the figures
};

/// A click (the ray's too).
struct CoverageClick {
    enum class Kind : std::uint8_t { Tab, System, Close };
    Kind kind = Kind::Close;
    CoverageTab tab = CoverageTab::Games;
    /// The row's place in the list shown (System).
    int index = 0;

    bool operator==(const CoverageClick&) const = default;
};

class CoverageModel {
public:
    /// Opens on the systems of every game, asking for the figures (takeRequest()).
    void open();
    void close() noexcept;

    /// The figures are asked again on the next frame (a catalog still opening).
    void prepare() { m_request = CoverageCommand::Load; }
    [[nodiscard]] std::optional<CoverageCommand> takeRequest() noexcept;

    /// A UI command while open; the scrolling of a system's details is the screen's.
    [[nodiscard]] std::optional<CoverageCommand> apply(UiCommand command);
    [[nodiscard]] std::optional<CoverageCommand> click(const CoverageClick& click);

    void setContent(CoverageContent content);
    void setUnavailable();

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] CoverageState state() const noexcept { return m_state; }
    [[nodiscard]] CoverageView view() const noexcept { return m_view; }
    [[nodiscard]] CoverageTab tab() const noexcept { return m_tab; }
    [[nodiscard]] const CoverageContent& content() const noexcept { return m_content; }
    /// The rows of the population shown.
    [[nodiscard]] const std::vector<CoverageRow>& rows() const noexcept { return m_content.table(m_tab).systems; }
    [[nodiscard]] std::size_t focused() const noexcept { return m_focused; }
    /// The system the focus stands on (or shows); null when none.
    [[nodiscard]] const CoverageRow* system() const noexcept;
    /// The clips of the system shown.
    [[nodiscard]] std::vector<CoverageClip> systemClips() const;
    /// The element the focus stands on ("" none).
    [[nodiscard]] std::string focusedElement() const;
    [[nodiscard]] static std::string rowElement(std::size_t index);
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void moveTo(std::size_t index);
    void step(int delta);
    void setTab(CoverageTab tab);

    bool m_open = false;
    CoverageState m_state = CoverageState::Loading;
    CoverageView m_view = CoverageView::Systems;
    CoverageTab m_tab = CoverageTab::Games;
    CoverageContent m_content;
    std::size_t m_focused = 0;
    /// The system the focus stands on, by name: the other population lists it elsewhere.
    std::string m_focusedName;
    std::optional<CoverageCommand> m_request;
    std::uint64_t m_revision = 0;
};

/// `share` (0 to 1) as the screen shows it: "90,9", one decimal and a comma; "-" for nothing.
[[nodiscard]] std::string shareText(std::optional<double> share);

} // namespace rv::ui
