// The on-screen keyboard's model (ADR-0018 §8): the grid, its moves and wrapping, what each key and command asks, UTF-8 of
// the accented letters, clicks mirroring the keys.

#include <rv/ui/keyboard_model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <rv/core/utf8.hpp>

using rv::ui::KeyboardKey;
using rv::ui::KeyboardKeyKind;
using rv::ui::KeyboardModel;
using rv::ui::KeyboardOutput;
using rv::ui::UiCommand;
namespace keyboard = rv::ui::keyboard;

namespace {

using Kind = KeyboardOutput::Kind;

std::size_t indexOf(std::string_view text) {
    const auto keys = rv::ui::keyboardKeys();
    const auto found = std::find_if(keys.begin(), keys.end(), [&](const KeyboardKey& key) { return key.text == text; });
    REQUIRE(found != keys.end());
    return static_cast<std::size_t>(found - keys.begin());
}

std::size_t indexOf(KeyboardKeyKind kind) {
    const auto keys = rv::ui::keyboardKeys();
    const auto found = std::find_if(keys.begin(), keys.end(), [&](const KeyboardKey& key) { return key.kind == kind; });
    REQUIRE(found != keys.end());
    return static_cast<std::size_t>(found - keys.begin());
}

void move(KeyboardModel& model, UiCommand command, int times = 1) {
    for (int index = 0; index < times; ++index) {
        CHECK_FALSE(model.apply(command).has_value());
    }
}

} // namespace

TEST_CASE("ui/keyboard: every row covers the grid's columns, each key once", "[ui][keyboard]") {
    for (int row = 0; row < keyboard::kRows; ++row) {
        int column = 0;
        for (const KeyboardKey& key : rv::ui::keyboardKeys()) {
            if (key.row == row) {
                CHECK(key.column == column);
                column += key.span;
            }
        }
        CHECK(column == keyboard::kColumns);
    }
    std::set<std::string_view> characters;
    for (const KeyboardKey& key : rv::ui::keyboardKeys()) {
        if (key.kind == KeyboardKeyKind::Character) {
            CHECK(characters.insert(key.text).second);
        }
    }
}

TEST_CASE("ui/keyboard: the digits, the letters, the Italian accented letters, each valid UTF-8", "[ui][keyboard]") {
    for (const std::string_view text : {"0", "9", "a", "z", "à", "è", "é", "ì", "ò", "ù"}) {
        CHECK(rv::ui::keyboardKeys()[indexOf(text)].kind == KeyboardKeyKind::Character);
    }
    for (const KeyboardKey& key : rv::ui::keyboardKeys()) {
        CHECK(rv::core::isValidUtf8(key.text));
    }
    // Two bytes each, as Windows-1252 would not be: U+00E0 and U+00F9.
    CHECK(rv::ui::keyboardKeys()[indexOf("à")].text == std::string_view("\xC3\xA0"));
    CHECK(rv::ui::keyboardKeys()[indexOf("ù")].text == std::string_view("\xC3\xB9"));
}

TEST_CASE("ui/keyboard: nothing happens while closed; it opens on the first letter", "[ui][keyboard]") {
    KeyboardModel model;
    CHECK_FALSE(model.isOpen());
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK_FALSE(model.click(0).has_value());
    CHECK_FALSE(model.clickOutside().has_value());
    model.open();
    CHECK(model.isOpen());
    CHECK(model.selectedKey().text == "q");
}

TEST_CASE("ui/keyboard: the moves wrap in both directions", "[ui][keyboard]") {
    KeyboardModel model;
    model.open();
    move(model, UiCommand::Left);
    CHECK(model.selectedKey().text == "p");
    move(model, UiCommand::Right);
    CHECK(model.selectedKey().text == "q");
    move(model, UiCommand::Up);
    CHECK(model.selectedKey().text == "1");
    move(model, UiCommand::Up);
    CHECK(model.selectedKey().kind == KeyboardKeyKind::Space); // over the top: the bottom row
    move(model, UiCommand::Down);
    CHECK(model.selectedKey().text == "1");
    // The wide keys' row wraps by keys, not by columns.
    move(model, UiCommand::Up);
    move(model, UiCommand::Left);
    CHECK(model.selectedKey().kind == KeyboardKeyKind::Confirm);
    move(model, UiCommand::Left);
    CHECK(model.selectedKey().kind == KeyboardKeyKind::Delete);
    move(model, UiCommand::Left);
    CHECK(model.selectedKey().kind == KeyboardKeyKind::Space);
    move(model, UiCommand::Right);
    CHECK(model.selectedKey().kind == KeyboardKeyKind::Delete);
}

TEST_CASE("ui/keyboard: the column survives the row of wide keys", "[ui][keyboard]") {
    KeyboardModel model;
    model.open();
    move(model, UiCommand::Right, 8); // o, column 8
    CHECK(model.selectedKey().text == "o");
    move(model, UiCommand::Up, 2); // over the digits, to Conferma
    CHECK(model.selectedKey().kind == KeyboardKeyKind::Confirm);
    move(model, UiCommand::Up);
    CHECK(model.selectedKey().text == "?"); // back at column 8
    move(model, UiCommand::Down, 2);
    CHECK(model.selectedKey().text == "9");
}

TEST_CASE("ui/keyboard: a move changes the revision, a command it does not use nothing", "[ui][keyboard]") {
    KeyboardModel model;
    model.open();
    const std::uint64_t before = model.revision();
    move(model, UiCommand::Right);
    CHECK(model.revision() != before);
    const std::uint64_t moved = model.revision();
    CHECK_FALSE(model.apply(UiCommand::Media).has_value());
    CHECK_FALSE(model.apply(UiCommand::FilterNext).has_value());
    CHECK_FALSE(model.apply(UiCommand::PageDown).has_value());
    CHECK(model.revision() == moved);
}

TEST_CASE("ui/keyboard: Activate presses the selected key", "[ui][keyboard]") {
    KeyboardModel model;
    model.open();
    CHECK(model.apply(UiCommand::Activate) == KeyboardOutput{Kind::Type, "q"});
    CHECK(model.isOpen());
    move(model, UiCommand::Up, 2);
    CHECK(model.apply(UiCommand::Activate) == KeyboardOutput{Kind::Type, " "});
    move(model, UiCommand::Right);
    CHECK(model.apply(UiCommand::Activate) == KeyboardOutput{Kind::Delete, {}});
    CHECK(model.isOpen());
    move(model, UiCommand::Right);
    CHECK(model.apply(UiCommand::Activate) == KeyboardOutput{Kind::Confirm, {}});
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/keyboard: X deletes, B closes, Start confirms", "[ui][keyboard]") {
    KeyboardModel model;
    model.open();
    CHECK(model.apply(UiCommand::Info) == KeyboardOutput{Kind::Delete, {}});
    CHECK(model.isOpen());
    CHECK(model.apply(UiCommand::Back) == KeyboardOutput{Kind::Close, {}});
    CHECK_FALSE(model.isOpen());
    model.open();
    CHECK(model.apply(UiCommand::Confirm) == KeyboardOutput{Kind::Confirm, {}});
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/keyboard: a click selects and presses its key; beside the panel it closes", "[ui][keyboard]") {
    KeyboardModel model;
    model.open();
    CHECK(model.click(indexOf("è")) == KeyboardOutput{Kind::Type, "è"});
    CHECK(model.selectedKey().text == "è");
    CHECK(model.click(indexOf(KeyboardKeyKind::Delete)) == KeyboardOutput{Kind::Delete, {}});
    CHECK_FALSE(model.click(rv::ui::keyboardKeys().size()).has_value()); // no such key
    CHECK(model.isOpen());
    CHECK(model.clickOutside() == KeyboardOutput{Kind::Close, {}});
    CHECK_FALSE(model.isOpen());
    model.open();
    CHECK(model.click(indexOf(KeyboardKeyKind::Confirm)) == KeyboardOutput{Kind::Confirm, {}});
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/keyboard: reopening starts from the first letter", "[ui][keyboard]") {
    KeyboardModel model;
    model.open();
    move(model, UiCommand::Down, 3);
    static_cast<void>(model.apply(UiCommand::Back));
    model.open();
    CHECK(model.selectedKey().text == "q");
}
