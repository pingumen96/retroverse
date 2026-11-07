// The menu's model (ADR-0018 §1): its buttons (I7 step 8: Copertura media and Libreria before Esci), Esci's one
// confirmation, Back's levels, clicks mirroring the keys.

#include <rv/ui/system_menu_model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using rv::ui::SystemMenuButton;
using rv::ui::SystemMenuCommand;
using rv::ui::SystemMenuModel;
using rv::ui::SystemMenuView;
using rv::ui::UiCommand;

TEST_CASE("ui/system_menu: opens on Riprendi; the focus stops at the ends", "[ui][system_menu]") {
    SystemMenuModel menu;
    CHECK_FALSE(menu.apply(UiCommand::Down).has_value()); // closed: nothing
    menu.open();
    CHECK(menu.isOpen());
    CHECK(menu.view() == SystemMenuView::Main);
    CHECK(menu.focused() == SystemMenuButton::Resume);
    CHECK(menu.focusedElement() == "sm-resume");
    static_cast<void>(menu.apply(UiCommand::Up));
    CHECK(menu.focused() == SystemMenuButton::Resume);
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focused() == SystemMenuButton::Settings);
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focused() == SystemMenuButton::Emulators);
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focused() == SystemMenuButton::Statistics);
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focused() == SystemMenuButton::Coverage);
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focused() == SystemMenuButton::Library);
    static_cast<void>(menu.apply(UiCommand::Down));
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focused() == SystemMenuButton::Quit);
}

TEST_CASE("ui/system_menu: Riprendi and Back close it", "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    CHECK(menu.apply(UiCommand::Activate) == SystemMenuCommand::Close);
    CHECK_FALSE(menu.isOpen());
    menu.open();
    CHECK(menu.apply(UiCommand::Back) == SystemMenuCommand::Close);
    CHECK_FALSE(menu.isOpen());
}

TEST_CASE("ui/system_menu: Impostazioni asks for its screen and stays open under it", "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.apply(UiCommand::Activate) == SystemMenuCommand::OpenSettings);
    CHECK(menu.isOpen());
}

TEST_CASE("ui/system_menu: Emulatori asks for its screen and stays open under it", "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    static_cast<void>(menu.apply(UiCommand::Down));
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.apply(UiCommand::Activate) == SystemMenuCommand::OpenEmulators);
    CHECK(menu.isOpen());
    CHECK(menu.click(SystemMenuButton::Emulators) == SystemMenuCommand::OpenEmulators);
}

TEST_CASE("ui/system_menu: Statistiche asks for its screen and stays open under it", "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    for (int press = 0; press < 3; ++press) {
        static_cast<void>(menu.apply(UiCommand::Down));
    }
    CHECK(menu.focusedElement() == "sm-statistics");
    CHECK(menu.apply(UiCommand::Activate) == SystemMenuCommand::OpenStatistics);
    CHECK(menu.isOpen());
    CHECK(menu.click(SystemMenuButton::Statistics) == SystemMenuCommand::OpenStatistics);
}

TEST_CASE("ui/system_menu: Copertura media and Libreria ask for their screens and stay open under them",
          "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    for (int press = 0; press < 4; ++press) {
        static_cast<void>(menu.apply(UiCommand::Down));
    }
    CHECK(menu.focusedElement() == "sm-coverage");
    CHECK(menu.apply(UiCommand::Activate) == SystemMenuCommand::OpenCoverage);
    CHECK(menu.isOpen());
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focusedElement() == "sm-library");
    CHECK(menu.apply(UiCommand::Activate) == SystemMenuCommand::OpenLibrary);
    CHECK(menu.isOpen());
    CHECK(menu.click(SystemMenuButton::Coverage) == SystemMenuCommand::OpenCoverage);
    CHECK(menu.click(SystemMenuButton::Library) == SystemMenuCommand::OpenLibrary);
    CHECK(rv::ui::toString(SystemMenuCommand::OpenCoverage) == "open-coverage");
    CHECK(rv::ui::toString(SystemMenuCommand::OpenLibrary) == "open-library");
}

TEST_CASE("ui/system_menu: Esci asks once, Annulla focused", "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    for (int press = 0; press < 6; ++press) {
        static_cast<void>(menu.apply(UiCommand::Down));
    }
    CHECK_FALSE(menu.apply(UiCommand::Activate).has_value());
    CHECK(menu.view() == SystemMenuView::Confirm);
    CHECK(menu.focused() == SystemMenuButton::Stay);
    CHECK(menu.buttons() == std::vector<SystemMenuButton>{SystemMenuButton::Stay, SystemMenuButton::ConfirmQuit});

    // A second press of the same button leaves nothing: back on Esci.
    CHECK_FALSE(menu.apply(UiCommand::Activate).has_value());
    CHECK(menu.view() == SystemMenuView::Main);
    CHECK(menu.focused() == SystemMenuButton::Quit);

    static_cast<void>(menu.apply(UiCommand::Activate));
    static_cast<void>(menu.apply(UiCommand::Down));
    CHECK(menu.focused() == SystemMenuButton::ConfirmQuit);
    CHECK(menu.apply(UiCommand::Activate) == SystemMenuCommand::Quit);
}

TEST_CASE("ui/system_menu: Back leaves the confirmation, not the menu", "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    static_cast<void>(menu.click(SystemMenuButton::Quit));
    CHECK(menu.view() == SystemMenuView::Confirm);
    CHECK_FALSE(menu.apply(UiCommand::Back).has_value());
    CHECK(menu.isOpen());
    CHECK(menu.view() == SystemMenuView::Main);
}

TEST_CASE("ui/system_menu: a click presses a button shown, as its key does", "[ui][system_menu]") {
    SystemMenuModel menu;
    menu.open();
    CHECK_FALSE(menu.click(SystemMenuButton::ConfirmQuit).has_value()); // not shown yet
    CHECK(menu.view() == SystemMenuView::Main);
    CHECK(menu.click(SystemMenuButton::Settings) == SystemMenuCommand::OpenSettings);
    CHECK(menu.focused() == SystemMenuButton::Settings);
    static_cast<void>(menu.click(SystemMenuButton::Quit));
    CHECK(menu.click(SystemMenuButton::ConfirmQuit) == SystemMenuCommand::Quit);
}

TEST_CASE("ui/system_menu: names", "[ui][system_menu]") {
    for (const SystemMenuButton button :
         {SystemMenuButton::Resume, SystemMenuButton::Settings, SystemMenuButton::Emulators, SystemMenuButton::Statistics,
          SystemMenuButton::Coverage, SystemMenuButton::Library, SystemMenuButton::Quit, SystemMenuButton::ConfirmQuit,
          SystemMenuButton::Stay}) {
        CHECK(rv::ui::systemMenuButtonFromName(rv::ui::toString(button)) == button);
        CHECK_FALSE(rv::ui::elementIdOf(button).empty());
    }
    CHECK_FALSE(rv::ui::systemMenuButtonFromName("nope").has_value());
    CHECK(rv::ui::toString(SystemMenuCommand::OpenSettings) == "open-settings");
    CHECK(rv::ui::toString(SystemMenuView::Confirm) == "confirm");
}
