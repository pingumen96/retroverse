#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The on-screen keyboard (ADR-0018 §8; assets/ui/keyboard.rml through ui::KeyboardScreen): a panel over a text field that
// a pad's Activate, or VR's ray, opens. The grid is this file's, one table for the keys typed and the keys shown: digits,
// lower-case letters, the Italian accented letters, what titles hold, then space, delete and confirm. The model holds no
// text: the field does, and the application applies what the model answers to it.
//
// | Command                | What it does                                                                   |
// |------------------------|--------------------------------------------------------------------------------|
// | Up, Down               | the row above or below, keeping the column; wraps                              |
// | Left, Right            | the key beside, wrapping within the row                                        |
// | Activate               | the selected key: types, deletes or confirms                                   |
// | Info (the pad's X)     | deletes                                                                        |
// | Back                   | closes: the text stays, the focus on the field                                 |
// | Confirm (the pad Start)| confirms: closes, on to the results as Enter does in the field                 |

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace keyboard {
inline constexpr std::string_view kDocumentPath = "ui/keyboard.rml";
inline constexpr std::string_view kDataModel = "keyboard";
inline constexpr std::string_view kPressEvent = "press";
inline constexpr std::string_view kOutsideEvent = "outside";
inline constexpr std::string_view kPanelId = "keyboard";
inline constexpr std::string_view kGridId = "keyboard-grid";
/// The grid's size: a key takes `span` columns of kColumns.
inline constexpr int kColumns = 10;
inline constexpr int kRows = 6;
} // namespace keyboard

enum class KeyboardKeyKind : std::uint8_t { Character, Space, Delete, Confirm };
/// "character", "space", "delete", "confirm" (the document's key.kind).
[[nodiscard]] std::string_view toString(KeyboardKeyKind kind) noexcept;

/// A key of the grid.
struct KeyboardKey {
    KeyboardKeyKind kind = KeyboardKeyKind::Character;
    /// What it types, UTF-8 ("" for delete and confirm); a space for the space bar.
    std::string_view text;
    int row = 0;
    int column = 0;
    int span = 1;
};

/// Every key, by row and then by column.
[[nodiscard]] std::span<const KeyboardKey> keyboardKeys() noexcept;

/// What the keyboard asks of the application.
struct KeyboardOutput {
    enum class Kind : std::uint8_t {
        Type,    ///< `text` goes into the field
        Delete,  ///< the character before the caret goes
        Confirm, ///< it closed itself: the text is done
        Close,   ///< it closed itself: back to the field
    };
    Kind kind = Kind::Close;
    std::string text;

    bool operator==(const KeyboardOutput&) const = default;
};
/// "type", "delete", "confirm", "close".
[[nodiscard]] std::string_view toString(KeyboardOutput::Kind kind) noexcept;

/// The keyboard's state: open, the selected key. Main thread; no rendering. Every change of what the panel shows
/// increments revision().
class KeyboardModel {
public:
    /// Opens on the first letter.
    void open();
    void close() noexcept;

    /// A UI command. What to do of it, if anything; commands the keyboard does not use do nothing.
    [[nodiscard]] std::optional<KeyboardOutput> apply(UiCommand command);
    /// A click on the key `index` of keyboardKeys(): it is selected and pressed.
    [[nodiscard]] std::optional<KeyboardOutput> click(std::size_t index);
    /// A click outside the panel: closes it, as Back does.
    [[nodiscard]] std::optional<KeyboardOutput> clickOutside();

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    /// Index in keyboardKeys().
    [[nodiscard]] std::size_t selected() const noexcept { return m_selected; }
    [[nodiscard]] const KeyboardKey& selectedKey() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void select(std::size_t index) noexcept;
    void moveColumn(int step) noexcept;
    void moveRow(int step) noexcept;
    [[nodiscard]] std::optional<KeyboardOutput> press(const KeyboardKey& key);
    [[nodiscard]] KeyboardOutput closing(KeyboardOutput::Kind kind) noexcept;

    bool m_open = false;
    std::size_t m_selected = 0;
    /// The column Up and Down keep: a wide key's row takes it and gives it back.
    int m_column = 0;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
