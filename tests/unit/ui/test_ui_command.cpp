#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>

#include <rv/ui/ui_command.hpp>

using rv::ui::commandFor;
using rv::ui::directionOf;
using rv::ui::kUiCommandCount;
using rv::ui::NavDirection;
using rv::ui::toString;
using rv::ui::UiCommand;
using rv::ui::UiCommandList;
using rv::ui::uiCommandFromName;

TEST_CASE("ui/ui_command: every command has a stable name that parses back", "[ui][ui_command]") {
    for (std::size_t index = 0; index < kUiCommandCount; ++index) {
        const auto command = static_cast<UiCommand>(index);
        CHECK(uiCommandFromName(toString(command)) == command);
    }
    CHECK(toString(UiCommand::Activate) == "activate");
    CHECK(toString(UiCommand::Media) == "media");
    CHECK(toString(UiCommand::Count) == "unknown");
    CHECK_FALSE(uiCommandFromName("Activate").has_value()); // exact, case-sensitive
    CHECK_FALSE(uiCommandFromName("").has_value());
}

TEST_CASE("ui/ui_command: focus moves map to directions and back", "[ui][ui_command]") {
    for (const NavDirection direction :
         {NavDirection::Up, NavDirection::Down, NavDirection::Left, NavDirection::Right}) {
        CHECK(directionOf(commandFor(direction)) == direction);
    }
    CHECK(toString(NavDirection::Left) == "left");
    CHECK_FALSE(directionOf(UiCommand::Activate).has_value());
    CHECK_FALSE(directionOf(UiCommand::Back).has_value());
}

TEST_CASE("ui/ui_command: a command list keeps the order and drops what exceeds its capacity", "[ui][ui_command]") {
    UiCommandList list;
    CHECK(list.empty());
    CHECK(list.push(UiCommand::Down));
    CHECK(list.push(UiCommand::Activate));
    REQUIRE(list.size() == 2);
    CHECK(list.commands()[0] == UiCommand::Down);
    CHECK(list.commands()[1] == UiCommand::Activate);
    CHECK(list.contains(UiCommand::Activate));
    CHECK_FALSE(list.contains(UiCommand::Back));

    for (std::size_t index = list.size(); index < UiCommandList::kCapacity; ++index) {
        CHECK(list.push(UiCommand::Up));
    }
    CHECK_FALSE(list.push(UiCommand::Back));
    CHECK(list.size() == UiCommandList::kCapacity);
    CHECK(list.dropped() == 1);

    list.clear();
    CHECK(list.empty());
    CHECK(list.dropped() == 0);
}
