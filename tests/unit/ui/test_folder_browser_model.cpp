// The folder browser's model (ADR-0018 §4): listings answered by serial, a folder that vanished or a volume that never
// answers leaving every button working, Usa choosing only what was listed, clicks mirroring the keys.

#include <rv/ui/folder_browser_model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <fmt/format.h>

using rv::ui::FolderBrowserClick;
using rv::ui::FolderBrowserFailure;
using rv::ui::FolderBrowserFocus;
using rv::ui::FolderBrowserModel;
using rv::ui::FolderBrowserState;
using rv::ui::FolderRow;
using rv::ui::UiCommand;
namespace fs = std::filesystem;
namespace browser = rv::ui::folder_browser;

namespace {

std::vector<FolderRow> rowsOf(const fs::path& parent, std::size_t count) {
    std::vector<FolderRow> rows;
    for (std::size_t index = 0; index < count; ++index) {
        const std::string name = fmt::format("folder{:03}", index);
        rows.push_back(FolderRow{parent / name, name});
    }
    return rows;
}

/// Answers the request pending with `count` folders.
void answer(FolderBrowserModel& model, std::size_t count) {
    const auto request = model.takeRequest();
    REQUIRE(request.has_value());
    model.setListing(request->serial, rowsOf(request->folder, count));
}

} // namespace

TEST_CASE("ui/folder_browser: opens where it is told and lists by serial", "[ui][folder_browser]") {
    FolderBrowserModel model;
    CHECK_FALSE(model.isOpen());
    model.open(fs::path("C:\\games\\roms"));
    CHECK(model.state() == FolderBrowserState::Loading);
    const auto request = model.takeRequest();
    REQUIRE(request.has_value());
    CHECK(request->folder == fs::path("C:\\games\\roms"));
    CHECK_FALSE(model.takeRequest().has_value()); // once
    CHECK_FALSE(model.canUse());                  // not listed yet

    model.setListing(request->serial + 1, rowsOf("x", 2)); // nobody asked this
    CHECK(model.state() == FolderBrowserState::Loading);
    model.setListing(request->serial, rowsOf(request->folder, 2));
    CHECK(model.state() == FolderBrowserState::Listed);
    CHECK(model.focus() == FolderBrowserFocus::Row);
    CHECK(model.canUse());
    CHECK(model.focusedElement() == "fb-row-0");
}

TEST_CASE("ui/folder_browser: an answer that comes after the user left is dropped", "[ui][folder_browser]") {
    FolderBrowserModel model;
    model.open(fs::path("D:\\slow"));
    const auto slow = model.takeRequest();
    REQUIRE(slow.has_value());
    // The volume does not answer: going up works all the same.
    CHECK(model.state() == FolderBrowserState::Loading);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value()); // the row up: focused while it loads
    const auto up = model.takeRequest();
    REQUIRE(up.has_value());
    CHECK(up->folder == fs::path("D:\\"));
    model.setListing(up->serial, rowsOf(up->folder, 1));
    model.setFailure(slow->serial, FolderBrowserFailure::Unreachable); // too late
    model.setListing(slow->serial, rowsOf(slow->folder, 5));           // too late
    CHECK(model.state() == FolderBrowserState::Listed);
    CHECK(model.folder() == fs::path("D:\\"));
    CHECK(model.count() == 1);
}

TEST_CASE("ui/folder_browser: over a vanished folder the row up and Annulla stay; Usa chooses nothing",
          "[ui][folder_browser]") {
    FolderBrowserModel model;
    model.open(fs::path("E:\\gone\\away"));
    const auto request = model.takeRequest();
    REQUIRE(request.has_value());
    model.setFailure(request->serial, FolderBrowserFailure::Missing);
    CHECK(model.state() == FolderBrowserState::Failed);
    REQUIRE(model.failure().has_value());
    CHECK(*model.failure() == FolderBrowserFailure::Missing);
    CHECK(model.focus() == FolderBrowserFocus::Up);
    CHECK_FALSE(model.canUse());
    CHECK_FALSE(model.apply(UiCommand::Info).has_value()); // Usa's shortcut
    CHECK_FALSE(model.click(FolderBrowserClick{FolderBrowserClick::Kind::Use}).has_value());
    CHECK(model.isOpen());
    CHECK(model.state() == FolderBrowserState::Failed);

    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.state() == FolderBrowserState::Loading);
    CHECK_FALSE(model.failure().has_value());
    CHECK(model.folder() == fs::path("E:\\gone"));
}

TEST_CASE("ui/folder_browser: above a root stand the drives, which Usa cannot choose", "[ui][folder_browser]") {
    FolderBrowserModel model;
    model.open(fs::path("C:\\"));
    answer(model, 0);
    CHECK(model.focus() == FolderBrowserFocus::Use); // nothing to enter: Usa
    static_cast<void>(model.apply(UiCommand::Right));
    static_cast<void>(model.apply(UiCommand::Right));
    CHECK(model.focus() == FolderBrowserFocus::Up);
    static_cast<void>(model.apply(UiCommand::Activate));
    CHECK(model.atDrives());
    answer(model, 3);
    CHECK_FALSE(model.canUse());
    static_cast<void>(model.apply(UiCommand::Right));
    CHECK(model.focus() == FolderBrowserFocus::Cancel); // Usa is skipped
    static_cast<void>(model.apply(UiCommand::Up));
    CHECK(model.focus() == FolderBrowserFocus::Row);
    static_cast<void>(model.apply(UiCommand::Up));
    CHECK(model.focus() == FolderBrowserFocus::Row); // no row up at the drives
}

TEST_CASE("ui/folder_browser: a window of rows over a long list", "[ui][folder_browser]") {
    FolderBrowserModel model;
    model.open(fs::path("C:\\many"));
    answer(model, 30);
    CHECK(model.rows().size() == browser::kVisibleRows);
    static_cast<void>(model.apply(UiCommand::PageDown));
    static_cast<void>(model.apply(UiCommand::PageDown));
    CHECK(model.selected() == 2 * browser::kVisibleRows);
    CHECK(model.first() == 2 * browser::kVisibleRows + 1 - browser::kVisibleRows);
    CHECK(model.focusedElement() == fmt::format("fb-row-{}", browser::kVisibleRows - 1));
    static_cast<void>(model.apply(UiCommand::End));
    CHECK(model.selected() == 29);
    CHECK(model.rows().back().name == "folder029");
    static_cast<void>(model.apply(UiCommand::Home));
    CHECK(model.selected() == 0);
    CHECK(model.first() == 0);

    model.scroll(5);
    CHECK(model.first() == 5);
    CHECK(model.selected() == 5); // stays in view
    model.scroll(1000);
    CHECK(model.first() == 30 - browser::kVisibleRows);
    model.scroll(-1000);
    CHECK(model.first() == 0);
}

TEST_CASE("ui/folder_browser: entering, choosing, cancelling; clicks mirror the keys", "[ui][folder_browser]") {
    FolderBrowserModel keys;
    keys.open(fs::path("C:\\games"));
    answer(keys, 3);
    static_cast<void>(keys.apply(UiCommand::Down));
    static_cast<void>(keys.apply(UiCommand::Activate));
    CHECK(keys.folder() == fs::path("C:\\games\\folder001"));
    answer(keys, 0);
    const auto chosen = keys.apply(UiCommand::Activate); // Usa, focused on an empty folder
    REQUIRE(chosen.has_value());
    REQUIRE(chosen->chosen.has_value());
    CHECK(*chosen->chosen == fs::path("C:\\games\\folder001"));
    CHECK_FALSE(keys.isOpen());

    FolderBrowserModel clicks;
    clicks.open(fs::path("C:\\games"));
    answer(clicks, 3);
    FolderBrowserClick row{FolderBrowserClick::Kind::Row};
    row.index = 1;
    static_cast<void>(clicks.click(row));
    CHECK(clicks.folder() == fs::path("C:\\games\\folder001"));
    answer(clicks, 0);
    const auto used = clicks.click(FolderBrowserClick{FolderBrowserClick::Kind::Use});
    REQUIRE(used.has_value());
    CHECK(*used->chosen == fs::path("C:\\games\\folder001"));

    FolderBrowserModel cancelled;
    cancelled.open(std::nullopt);
    answer(cancelled, 1);
    const auto back = cancelled.apply(UiCommand::Back);
    REQUIRE(back.has_value());
    CHECK_FALSE(back->chosen.has_value());
    CHECK_FALSE(cancelled.isOpen());
}

TEST_CASE("ui/folder browser: choosing a program: a row chooses it, no folder can be used", "[ui][folder_browser]") {
    FolderBrowserModel browser;
    browser.openForProgram(std::filesystem::path("C:/Emu"));
    const auto request = browser.takeRequest();
    REQUIRE(request.has_value());
    CHECK(request->programs);
    browser.setListing(request->serial, {FolderRow{"C:/Emu/cores", "cores", false},
                                         FolderRow{"C:/Emu/emu.exe", "emu.exe", true}});
    CHECK_FALSE(browser.canUse());
    CHECK_FALSE(browser.apply(UiCommand::Info).has_value());

    // A folder is entered, a program chosen.
    CHECK_FALSE(browser.apply(UiCommand::Down).has_value());
    const auto chosen = browser.apply(UiCommand::Activate);
    REQUIRE(chosen.has_value());
    CHECK(chosen->chosen == std::filesystem::path("C:/Emu/emu.exe"));
    CHECK_FALSE(browser.isOpen());

    browser.openForProgram(std::filesystem::path("C:/Emu"));
    const auto again = browser.takeRequest();
    REQUIRE(again.has_value());
    browser.setListing(again->serial, {FolderRow{"C:/Emu/cores", "cores", false}});
    CHECK_FALSE(browser.apply(UiCommand::Activate).has_value());
    CHECK(browser.isOpen());
    CHECK(browser.folder() == std::filesystem::path("C:/Emu/cores"));
    CHECK(browser.takeRequest()->programs); // a folder entered lists programs too

    // The folders' browser again: as it was.
    browser.open(std::filesystem::path("C:/Emu"));
    CHECK_FALSE(browser.takeRequest()->programs);
}
