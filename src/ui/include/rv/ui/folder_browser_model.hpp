#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The folder browser (ADR-0018 §4): paths are browsed, not typed, so that nothing needs a keyboard. The model holds where
// the browser stands and what it shows; listing is somebody else's, on a worker: the model asks (takeRequest()) and is
// answered by serial (setListing(), setFailure()), so an answer that comes late, from a volume the user left, is dropped,
// and while one is awaited every button still works: a slow or vanished volume never blocks a frame or the user.
//
// | Focus     | Up, Down, pages        | Activate           | Left, Right       | Info | Back   |
// |-----------|------------------------|--------------------|-------------------|------|--------|
// | a row     | the rows, the row up   | enters it          | Usa, Annulla      | Usa  | closes |
// | Usa       | the rows               | chooses the folder | Annulla, the rows | Usa  | closes |
// | Annulla   | the rows               | closes             | Usa, the rows     | Usa  | closes |
//
// The drives are its root (an empty path), which Usa cannot choose. Clicks press what the keys press.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace folder_browser {
/// The rows shown at a time (a fixed pool: the document never lays out thousands of rows).
inline constexpr std::size_t kVisibleRows = 8;
} // namespace folder_browser

/// A folder or drive listed.
struct FolderRow {
    std::filesystem::path path;
    std::string name;

    bool operator==(const FolderRow&) const = default;
};

enum class FolderBrowserState : std::uint8_t {
    Closed,
    Loading, ///< a listing was asked and is not answered yet
    Listed,
    Failed, ///< the folder cannot be listed: gone, out of reach
};
/// "closed", "loading", "listed", "failed".
[[nodiscard]] std::string_view toString(FolderBrowserState state) noexcept;

/// Why a folder could not be listed (the document's words): "missing", "unreachable", "unreadable".
enum class FolderBrowserFailure : std::uint8_t { Missing, Unreachable, Unreadable };
[[nodiscard]] std::string_view toString(FolderBrowserFailure failure) noexcept;

/// What the browser wants listed.
struct FolderListingRequest {
    std::uint64_t serial = 0;
    /// Empty: the drives.
    std::filesystem::path folder;

    bool operator==(const FolderListingRequest&) const = default;
};

/// What the focus stands on.
enum class FolderBrowserFocus : std::uint8_t { Up, Row, Use, Cancel };

/// A click (the ray's too).
struct FolderBrowserClick {
    enum class Kind : std::uint8_t { Up, Row, Use, Cancel };
    Kind kind = Kind::Up;
    /// The row's place among rows() (Kind::Row).
    std::size_t index = 0;

    bool operator==(const FolderBrowserClick&) const = default;
};

/// How it closed.
struct FolderBrowserResult {
    /// The folder chosen; nothing: cancelled.
    std::optional<std::filesystem::path> chosen;

    bool operator==(const FolderBrowserResult&) const = default;
};

class FolderBrowserModel {
public:
    /// Opens at `start` (nothing: the drives): Usa is offered once it was listed. Asks its listing.
    void open(const std::optional<std::filesystem::path>& start);
    void close() noexcept;

    /// The listing to ask, once.
    [[nodiscard]] std::optional<FolderListingRequest> takeRequest() noexcept;
    /// The folders of the listing `serial`; ignored unless it is the one awaited.
    void setListing(std::uint64_t serial, std::vector<FolderRow> rows);
    /// The listing `serial` failed; ignored unless it is the one awaited.
    void setFailure(std::uint64_t serial, FolderBrowserFailure failure);

    /// A UI command while open. How it closed, if it did.
    [[nodiscard]] std::optional<FolderBrowserResult> apply(UiCommand command);
    [[nodiscard]] std::optional<FolderBrowserResult> click(const FolderBrowserClick& click);
    /// The wheel: `lines` rows down (negative: up), the focus kept on what it stood on when it stays in view.
    void scroll(int lines);

    [[nodiscard]] bool isOpen() const noexcept { return m_state != FolderBrowserState::Closed; }
    [[nodiscard]] FolderBrowserState state() const noexcept { return m_state; }
    [[nodiscard]] const std::optional<FolderBrowserFailure>& failure() const noexcept { return m_failure; }
    /// Where it stands; empty: the drives.
    [[nodiscard]] const std::filesystem::path& folder() const noexcept { return m_folder; }
    [[nodiscard]] bool atDrives() const noexcept { return m_folder.empty(); }
    /// The rows in view, and how many there are.
    [[nodiscard]] std::vector<FolderRow> rows() const;
    [[nodiscard]] std::size_t count() const noexcept { return m_rows.size(); }
    /// The first row in view.
    [[nodiscard]] std::size_t first() const noexcept { return m_first; }
    /// The focused row among all of them.
    [[nodiscard]] std::size_t selected() const noexcept { return m_selected; }
    [[nodiscard]] FolderBrowserFocus focus() const noexcept { return m_focus; }
    /// Usa can choose what it stands on: a folder listed.
    [[nodiscard]] bool canUse() const noexcept { return m_state == FolderBrowserState::Listed && !atDrives(); }
    /// The element the focus stands on.
    [[nodiscard]] std::string focusedElement() const;
    /// The element of the row at `index` among rows().
    [[nodiscard]] static std::string rowElement(std::size_t index);
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void go(std::filesystem::path folder);
    void goUp();
    void moveRow(std::ptrdiff_t delta);
    void keepInView() noexcept;
    void focusOn(FolderBrowserFocus focus) noexcept;
    [[nodiscard]] std::optional<FolderBrowserResult> press();
    [[nodiscard]] std::optional<FolderBrowserResult> choose();

    FolderBrowserState m_state = FolderBrowserState::Closed;
    std::optional<FolderBrowserFailure> m_failure;
    std::filesystem::path m_folder;
    std::vector<FolderRow> m_rows;
    std::size_t m_first = 0;
    std::size_t m_selected = 0;
    FolderBrowserFocus m_focus = FolderBrowserFocus::Up;
    std::uint64_t m_serial = 0;
    std::optional<std::uint64_t> m_awaited;
    bool m_requestPending = false;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
