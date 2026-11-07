#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The library screen (ADR-0018 §14; assets/ui/library.rml through ui::LibraryScreen): five sections. Origini: each root's
// state and watch mode (the file watcher's, I7 step 7), the last import, "Aggiorna libreria". Problemi: the last import's
// issues, grouped by kind. Rimozioni: the systems the mass-disappearance guard holds, and "Applica le rimozioni", which
// asks before anything is removed. Da verificare: the review rows, each resolved to one of its own candidates or kept
// separate (catalog::resolveReview() on the worker; pairing asks first, since it cannot be undone). Impronte: the quick
// hashes the catalog lacks, and "Calcola impronte" on request: a pass on the worker that can be stopped and goes on with
// what is left. The figures come from the application (the catalog worker's answers, the watcher, the refresh and the
// pass as they stand), in text as they show; every word is the document's.
//
// | View            | Up, Down, pages             | Left, Right, filters | Activate                      | Back       |
// |-----------------|-----------------------------|----------------------|-------------------------------|------------|
// | Main            | the section's focusable ones | the section          | the focused button or review  | closes     |
// | Review          | its candidates, the buttons |                      | a candidate: ConfirmPair      | to Main    |
// | ConfirmPair     | Abbina, Annulla             |                      | Abbina: resolves              | to Review  |
// | ConfirmRemovals | Applica, Annulla            |                      | Applica: the refresh asked    | to Main    |
//
// In a section with nothing to focus (Problemi, an empty list), Up and Down are the screen's: the body scrolls. Clicks
// press what the keys press (VR's ray too). Every change of what shows increments revision().

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace library {
inline constexpr std::string_view kDocumentPath = "ui/library.rml";
inline constexpr std::string_view kDataModel = "library";
inline constexpr std::string_view kPanelId = "library-panel";
/// The region that scrolls.
inline constexpr std::string_view kBodyId = "lb-body";
/// The rows Page Up and Page Down move.
inline constexpr std::size_t kPageRows = 6;
/// The most systems an issue group names (the rest counted).
inline constexpr std::size_t kIssueSystems = 6;
} // namespace library

enum class LibraryState : std::uint8_t { Loading, Listed, Unavailable };
/// "loading", "listed", "unavailable".
[[nodiscard]] std::string_view toString(LibraryState state) noexcept;

enum class LibraryTab : std::uint8_t { Sources, Issues, Removals, Reviews, Hashes };
inline constexpr std::array<LibraryTab, 5> kLibraryTabs = {LibraryTab::Sources, LibraryTab::Issues, LibraryTab::Removals,
                                                           LibraryTab::Reviews, LibraryTab::Hashes};
/// "sources", "issues", "removals", "reviews", "hashes" (the click event's argument).
[[nodiscard]] std::string_view toString(LibraryTab tab) noexcept;
[[nodiscard]] std::optional<LibraryTab> libraryTabFromName(std::string_view name) noexcept;

enum class LibraryView : std::uint8_t { Main, Review, ConfirmPair, ConfirmRemovals };
/// "main", "review", "confirm-pair", "confirm-removals".
[[nodiscard]] std::string_view toString(LibraryView view) noexcept;

/// A folder of the library.
struct LibraryRoot {
    /// "esde-home", "rom", "media", or "system" (a system folder outside the ROM root).
    std::string kind;
    /// The system's name for "system".
    std::string system;
    /// The folder, as people read it ("" unknown).
    std::string path;
    /// "reachable", "unreachable", "unknown".
    std::string state = "unknown";
    /// The watcher's: "notify", "poll", "lost", "arming"; "off" when nothing watches it.
    std::string mode = "off";

    bool operator==(const LibraryRoot&) const = default;
};

/// The last import's issues of one code.
struct LibraryIssueGroup {
    std::string code;
    /// catalog::toString(IssueSeverity) of the worst.
    std::string severity;
    std::uint64_t count = 0;
    /// The systems named, at most library::kIssueSystems, ", " between them; how many more.
    std::string systems;
    std::uint64_t moreSystems = 0;

    bool operator==(const LibraryIssueGroup&) const = default;
};

/// A system the mass-disappearance guard holds.
struct LibraryHeld {
    std::string name;
    std::string fullName;
    /// Its files gone, of those present before (0 when the last import did not count it).
    std::uint64_t gone = 0;
    std::uint64_t present = 0;

    bool operator==(const LibraryHeld&) const = default;
};

/// A candidate of a review row: a new file the vanished game could be.
struct LibraryCandidate {
    std::string path;
    /// Its identity's GameId; "" when it has none present now, which resolveReview() refuses: not offered.
    std::string gameId;

    bool operator==(const LibraryCandidate&) const = default;
};

/// A review row (catalog::StoredReview, open).
struct LibraryReview {
    std::int64_t id = 0;
    std::string gameId;
    std::string system;
    /// The vanished game's title as the catalog last had it ("" none), its last path.
    std::string title;
    std::string vanished;
    std::vector<LibraryCandidate> candidates;

    bool operator==(const LibraryReview&) const = default;
};

/// The last import.
struct LibraryImport {
    bool known = false;
    /// When it ended, in the machine's time as it shows ("02/11/2026 14:11").
    std::string when;
    /// catalog::toString(ImportKind).
    std::string kind;
    /// It was of some systems (how many), and it accepted the guard's removals.
    std::uint64_t systems = 0;
    bool acceptRemovals = false;

    bool operator==(const LibraryImport&) const = default;
};

/// What the catalog worker read for the screen.
struct LibraryContent {
    std::vector<LibraryRoot> roots;
    /// The systems whose ROM folder the last import could not list.
    std::uint64_t unavailableSystems = 0;
    LibraryImport lastImport;
    std::vector<LibraryIssueGroup> issues;
    std::uint64_t issueCount = 0;
    std::vector<LibraryHeld> held;
    std::vector<LibraryReview> reviews;
    /// The present files (folders aside), and those with a current quick hash.
    std::uint64_t files = 0;
    std::uint64_t hashed = 0;

    bool operator==(const LibraryContent&) const = default;
};

/// What runs now: the refresh, the quick-hash pass, the watcher's modes.
struct LibraryLive {
    /// The catalog can refresh; one runs (its phase, done of total), how the last one ended ("" none: done, cancelled,
    /// busy, failed) and what it changed.
    bool canRefresh = false;
    bool refreshing = false;
    std::string phase;
    std::uint64_t refreshDone = 0;
    std::uint64_t refreshTotal = 0;
    std::string lastRefresh;
    std::uint64_t added = 0;
    std::uint64_t removed = 0;
    std::uint64_t guarded = 0;
    /// The pass: "idle", "running", "cancelling", "cancelled", "done", "busy" (an import held the lock), "failed"; where
    /// it is (files looked at of those it looks at, hashed) and what the last one left.
    std::string hash = "idle";
    std::uint64_t hashDone = 0;
    std::uint64_t hashTotal = 0;
    std::uint64_t hashHashed = 0;
    std::uint64_t hashUnreadable = 0;
    std::uint64_t hashRemaining = 0;
    /// The file watcher is on (its modes are in the roots).
    bool watching = false;
    /// Each root's mode, by its watcher name ("rom", "media", "gamelists", "system:<name>", ...).
    std::vector<std::pair<std::string, std::string>> modes;

    bool operator==(const LibraryLive&) const = default;
};

/// `roots` with the watcher's modes of `live`: "rom" for the ROM root, "media" for the media root, the worst of
/// "gamelists", "settings" and "custom-systems" for the ES-DE home ("lost" before "poll" before "arming" before
/// "notify"), "system:<name>" for a system folder outside the ROM root; "off" while nothing watches, or for a root the
/// watcher does not name.
[[nodiscard]] std::vector<LibraryRoot> withModes(std::vector<LibraryRoot> roots, const LibraryLive& live);

enum class LibraryCommandKind : std::uint8_t {
    Close,         ///< it closed itself
    Load,          ///< what the worker reads for it, again
    Refresh,       ///< "Aggiorna libreria"
    ApplyRemovals, ///< a refresh of `systems` that accepts the guard's removals (confirmed)
    Resolve,       ///< review `review` paired with `gameId` (confirmed), or kept separate ("")
    StartHash,     ///< "Calcola impronte": the pass, from what is left
    CancelHash,    ///< "Interrompi"
};
/// "close", "load", "refresh", "apply-removals", "resolve", "start-hash", "cancel-hash".
[[nodiscard]] std::string_view toString(LibraryCommandKind kind) noexcept;

struct LibraryCommand {
    LibraryCommandKind kind = LibraryCommandKind::Close;
    std::vector<std::string> systems;
    std::int64_t review = 0;
    std::string gameId;

    bool operator==(const LibraryCommand&) const = default;
};

enum class LibraryButton : std::uint8_t {
    Refresh,        ///< Aggiorna libreria (Origini)
    ApplyRemovals,  ///< Applica le rimozioni (Rimozioni)
    Hash,           ///< Calcola impronte, Interrompi, Riprendi (Impronte)
    KeepSeparate,   ///< Tieni separati (Review)
    ReviewCancel,   ///< Annulla (Review)
    Pair,           ///< Abbina (ConfirmPair)
    PairCancel,     ///< Annulla (ConfirmPair)
    Apply,          ///< Applica (ConfirmRemovals)
    RemovalsCancel, ///< Annulla (ConfirmRemovals)
};
/// "refresh", "apply-removals", "hash", "keep-separate", "review-cancel", "pair", "pair-cancel", "apply",
/// "removals-cancel" (the button event's argument).
[[nodiscard]] std::string_view toString(LibraryButton button) noexcept;
[[nodiscard]] std::optional<LibraryButton> libraryButtonFromName(std::string_view name) noexcept;
/// The button's element id ("lb-refresh" ...).
[[nodiscard]] std::string_view elementIdOf(LibraryButton button) noexcept;

/// A click (the ray's too).
struct LibraryClick {
    enum class Kind : std::uint8_t { Tab, Button, Review, Candidate, Close };
    Kind kind = Kind::Close;
    LibraryTab tab = LibraryTab::Sources;
    LibraryButton button = LibraryButton::Refresh;
    int index = 0;

    bool operator==(const LibraryClick&) const = default;
};

class LibraryModel {
public:
    /// Opens on Origini, asking what the worker reads (takeRequest()).
    void open();
    void close() noexcept;

    void prepare() { m_request = LibraryCommand{LibraryCommandKind::Load, {}, 0, {}}; }
    [[nodiscard]] std::optional<LibraryCommand> takeRequest() noexcept;

    /// A UI command while open, and the command for the application, if any. Up, Down and the pages in a section with
    /// nothing to focus return nothing and leave scrollsBody() true: the screen scrolls.
    [[nodiscard]] std::optional<LibraryCommand> apply(UiCommand command);
    [[nodiscard]] std::optional<LibraryCommand> click(const LibraryClick& click);

    void setContent(LibraryContent content);
    void setUnavailable();
    /// What runs now (every frame: nothing changes while it is the same). A refresh or a pass that ended asks for the
    /// content again.
    void setLive(LibraryLive live);
    /// How a resolution ended: "" resolved (back to Da verificare, the content asked again), else a key of the
    /// document ("refused", "conflict", "busy", "write").
    void resolved(std::string_view problem);

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] LibraryState state() const noexcept { return m_state; }
    [[nodiscard]] LibraryView view() const noexcept { return m_view; }
    [[nodiscard]] LibraryTab tab() const noexcept { return m_tab; }
    [[nodiscard]] const LibraryContent& content() const noexcept { return m_content; }
    [[nodiscard]] const LibraryLive& live() const noexcept { return m_live; }
    /// The review the Review views stand on; null when none.
    [[nodiscard]] const LibraryReview* review() const noexcept;
    /// The candidate a ConfirmPair stands on; null when none.
    [[nodiscard]] const LibraryCandidate* candidate() const noexcept;
    /// A resolution was asked and not answered yet; what the last one met ("" none).
    [[nodiscard]] bool resolving() const noexcept { return m_resolving; }
    [[nodiscard]] const std::string& problem() const noexcept { return m_problem; }
    /// The section shown has nothing to focus: Up and Down scroll the body.
    [[nodiscard]] bool scrollsBody() const;
    /// The element the focus stands on ("" none).
    [[nodiscard]] std::string focusedElement() const;
    [[nodiscard]] static std::string reviewElement(std::size_t index);
    [[nodiscard]] static std::string candidateElement(std::size_t index);
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    /// A focusable element of the view shown: a button, or a row (a review, a candidate) by its place.
    struct Item {
        std::optional<LibraryButton> button;
        std::size_t index = 0;

        bool operator==(const Item&) const = default;
    };

    [[nodiscard]] std::vector<Item> items() const;
    void focusFirst();
    void move(int delta);
    void show(LibraryView view);
    void setTab(LibraryTab tab);
    [[nodiscard]] std::optional<LibraryCommand> press(const Item& item);

    bool m_open = false;
    LibraryState m_state = LibraryState::Loading;
    LibraryView m_view = LibraryView::Main;
    LibraryTab m_tab = LibraryTab::Sources;
    LibraryContent m_content;
    LibraryLive m_live;
    Item m_focus;
    /// The review the Review views stand on (by id: the content may come again in another order), its candidate.
    std::int64_t m_review = 0;
    std::size_t m_candidate = 0;
    bool m_resolving = false;
    std::string m_problem;
    std::optional<LibraryCommand> m_request;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
