#include <rv/ui/keyboard_model.hpp>

#include <array>

namespace rv::ui {
namespace {

using Kind = KeyboardKeyKind;

/// One row of characters.
constexpr KeyboardKey character(std::string_view text, int row, int column) noexcept {
    return KeyboardKey{Kind::Character, text, row, column, 1};
}

constexpr std::size_t kCharacterRows = 5;
constexpr std::size_t kKeyCount = kCharacterRows * keyboard::kColumns + 3;

constexpr std::array<std::array<std::string_view, keyboard::kColumns>, kCharacterRows> kCharacters = {{
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", "'"},
    {"z", "x", "c", "v", "b", "n", "m", "-", ":", "."},
    {"à", "è", "é", "ì", "ò", "ù", ",", "!", "?", "&"},
}};

constexpr std::array<KeyboardKey, kKeyCount> makeKeys() noexcept {
    std::array<KeyboardKey, kKeyCount> keys{};
    std::size_t at = 0;
    for (std::size_t row = 0; row < kCharacters.size(); ++row) {
        for (std::size_t column = 0; column < kCharacters[row].size(); ++column) {
            keys[at++] = character(kCharacters[row][column], static_cast<int>(row), static_cast<int>(column));
        }
    }
    constexpr int kLast = static_cast<int>(kCharacterRows);
    keys[at++] = KeyboardKey{Kind::Space, " ", kLast, 0, 4};
    keys[at++] = KeyboardKey{Kind::Delete, "", kLast, 4, 3};
    keys[at++] = KeyboardKey{Kind::Confirm, "", kLast, 7, 3};
    return keys;
}

constexpr std::array<KeyboardKey, kKeyCount> kKeys = makeKeys();

/// The first letter, q.
constexpr std::size_t kFirstLetter = keyboard::kColumns;

/// The key of `row` that covers `column`.
[[nodiscard]] std::size_t keyAt(int row, int column) noexcept {
    for (std::size_t index = 0; index < kKeys.size(); ++index) {
        const KeyboardKey& key = kKeys[index];
        if (key.row == row && column >= key.column && column < key.column + key.span) {
            return index;
        }
    }
    return 0;
}

[[nodiscard]] constexpr int wrapped(int value, int count) noexcept {
    return (value % count + count) % count;
}

} // namespace

std::string_view toString(KeyboardKeyKind kind) noexcept {
    switch (kind) {
    case KeyboardKeyKind::Character:
        return "character";
    case KeyboardKeyKind::Space:
        return "space";
    case KeyboardKeyKind::Delete:
        return "delete";
    case KeyboardKeyKind::Confirm:
        return "confirm";
    }
    return "unknown";
}

std::string_view toString(KeyboardOutput::Kind kind) noexcept {
    switch (kind) {
    case KeyboardOutput::Kind::Type:
        return "type";
    case KeyboardOutput::Kind::Delete:
        return "delete";
    case KeyboardOutput::Kind::Confirm:
        return "confirm";
    case KeyboardOutput::Kind::Close:
        return "close";
    }
    return "unknown";
}

std::span<const KeyboardKey> keyboardKeys() noexcept {
    return kKeys;
}

void KeyboardModel::open() {
    m_open = true;
    m_selected = kFirstLetter;
    m_column = kKeys[kFirstLetter].column;
    ++m_revision;
}

void KeyboardModel::close() noexcept {
    if (m_open) {
        m_open = false;
        ++m_revision;
    }
}

const KeyboardKey& KeyboardModel::selectedKey() const noexcept {
    return kKeys[m_selected];
}

void KeyboardModel::select(std::size_t index) noexcept {
    if (m_selected != index) {
        m_selected = index;
        ++m_revision;
    }
}

void KeyboardModel::moveColumn(int step) noexcept {
    // From a wide key's far edge: the next key starts after it, the one before ends before it.
    const KeyboardKey& key = kKeys[m_selected];
    const int column = wrapped(step > 0 ? key.column + key.span : key.column - 1, keyboard::kColumns);
    select(keyAt(key.row, column));
    m_column = kKeys[m_selected].column;
}

void KeyboardModel::moveRow(int step) noexcept {
    select(keyAt(wrapped(kKeys[m_selected].row + step, keyboard::kRows), m_column));
}

KeyboardOutput KeyboardModel::closing(KeyboardOutput::Kind kind) noexcept {
    close();
    return KeyboardOutput{kind, {}};
}

std::optional<KeyboardOutput> KeyboardModel::press(const KeyboardKey& key) {
    switch (key.kind) {
    case KeyboardKeyKind::Character:
    case KeyboardKeyKind::Space:
        return KeyboardOutput{KeyboardOutput::Kind::Type, std::string(key.text)};
    case KeyboardKeyKind::Delete:
        return KeyboardOutput{KeyboardOutput::Kind::Delete, {}};
    case KeyboardKeyKind::Confirm:
        return closing(KeyboardOutput::Kind::Confirm);
    }
    return std::nullopt;
}

std::optional<KeyboardOutput> KeyboardModel::apply(UiCommand command) {
    if (!m_open) {
        return std::nullopt;
    }
    switch (command) {
    case UiCommand::Up:
        moveRow(-1);
        return std::nullopt;
    case UiCommand::Down:
        moveRow(1);
        return std::nullopt;
    case UiCommand::Left:
        moveColumn(-1);
        return std::nullopt;
    case UiCommand::Right:
        moveColumn(1);
        return std::nullopt;
    case UiCommand::Activate:
        return press(kKeys[m_selected]);
    case UiCommand::Info:
        return KeyboardOutput{KeyboardOutput::Kind::Delete, {}};
    case UiCommand::Back:
        return closing(KeyboardOutput::Kind::Close);
    case UiCommand::Confirm:
        return closing(KeyboardOutput::Kind::Confirm);
    default:
        return std::nullopt;
    }
}

std::optional<KeyboardOutput> KeyboardModel::click(std::size_t index) {
    if (!m_open || index >= kKeys.size()) {
        return std::nullopt;
    }
    select(index);
    m_column = kKeys[index].column;
    return press(kKeys[index]);
}

std::optional<KeyboardOutput> KeyboardModel::clickOutside() {
    if (!m_open) {
        return std::nullopt;
    }
    return closing(KeyboardOutput::Kind::Close);
}

} // namespace rv::ui
