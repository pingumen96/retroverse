// The tree walker on the situations of a real ROM or media folder (ADR-0008 §3): nested folders in a deterministic
// order, paths longer than MAX_PATH, junctions (also a loop), a folder that cannot be listed, hidden and leading-dot
// files, Unicode names, names without a UTF-8 form, cancellation and the root's errors.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/core/utf8.hpp>

#include "support/file_system.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::core::listTree;
using rv::core::TreeEntry;
using rv::core::TreeEntryKind;
using rv::core::walkTree;
using rv::core::WalkAction;

namespace {

void write(const fs::path& file, std::string_view content) {
    REQUIRE(rv::test::writeFileAnyLength(file, content).has_value());
}

std::vector<TreeEntry> walked(const fs::path& root, const rv::core::TreeWalkOptions& options = {}) {
    auto entries = listTree(root, options);
    INFO((entries ? std::string("listed") : entries.error().toString()));
    REQUIRE(entries.has_value());
    return std::move(*entries);
}

/// "<kind> <path>" per entry: F file, D directory, E error.
std::vector<std::string> outline(const std::vector<TreeEntry>& entries) {
    std::vector<std::string> lines;
    for (const TreeEntry& entry : entries) {
        const char* kind = entry.kind == TreeEntryKind::File        ? "F "
                           : entry.kind == TreeEntryKind::Directory ? "D "
                                                                    : "E ";
        lines.push_back(kind + entry.relativePath);
    }
    return lines;
}

const TreeEntry& find(const std::vector<TreeEntry>& entries, std::string_view path, TreeEntryKind kind) {
    const TreeEntry* found = nullptr;
    for (const TreeEntry& entry : entries) {
        if (entry.relativePath == path && entry.kind == kind) {
            found = &entry;
            break;
        }
    }
    INFO("entry " << path);
    REQUIRE(found != nullptr);
    return *found;
}

/// A drive letter with no drive behind it, or 0.
wchar_t unusedDriveLetter() {
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
        if ((drives & (1u << (letter - L'A'))) == 0) {
            return letter;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("core/tree_walk: nested folders, depth first, names in ordinal UTF-8 order", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    write(dir.file("b/2.txt"), "22");
    write(dir.file("a/sub/x.txt"), "xxx");
    write(dir.file("a/1.txt"), "1");
    write(dir.file("c.txt"), "");
    write(dir.file("B.txt"), "B");
    write(dir.file("a/sub/deeper/y.txt"), "yyyy");
    fs::create_directories(dir.file("empty"));

    const auto entries = walked(dir.path());
    CHECK(outline(entries) == std::vector<std::string>{"F B.txt", "D a", "F a/1.txt", "D a/sub", "D a/sub/deeper",
                                                       "F a/sub/deeper/y.txt", "F a/sub/x.txt", "D b", "F b/2.txt",
                                                       "F c.txt", "D empty"});
    CHECK(find(entries, "a/sub/deeper/y.txt", TreeEntryKind::File).size == 4);
    CHECK(find(entries, "c.txt", TreeEntryKind::File).size == 0);
    CHECK(find(entries, "a", TreeEntryKind::Directory).size == 0);
    CHECK(find(entries, "b/2.txt", TreeEntryKind::File).lastWriteTime == fs::last_write_time(dir.file("b/2.txt")));
    for (const TreeEntry& entry : entries) {
        CHECK_FALSE(entry.hidden);
        CHECK(entry.reparseTag == 0);
    }

    // The same walk through a relative root and a root with a trailing separator.
    struct CurrentDirectory {
        fs::path previous = fs::current_path();
        ~CurrentDirectory() { fs::current_path(previous); }
    };
    std::vector<TreeEntry> relative;
    {
        const CurrentDirectory restore;
        fs::current_path(dir.path());
        relative = walked(fs::path("a"));
    }
    CHECK(outline(relative) ==
          std::vector<std::string>{"F 1.txt", "D sub", "D sub/deeper", "F sub/deeper/y.txt", "F sub/x.txt"});
    CHECK(outline(walked(dir.path() / "a" / "")) == outline(relative));
}

TEST_CASE("core/tree_walk: what the visitor asks for", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    write(dir.file("a/1.txt"), "1");
    write(dir.file("a/2.txt"), "2");
    write(dir.file("b.txt"), "b");

    std::vector<std::string> seen;
    auto summary = walkTree(dir.path(), [&](const TreeEntry& entry) {
        seen.push_back(entry.relativePath);
        return entry.relativePath == "a" ? WalkAction::SkipChildren : WalkAction::Continue;
    });
    REQUIRE(summary.has_value());
    CHECK(seen == std::vector<std::string>{"a", "b.txt"});
    CHECK(summary->directories == 1);
    CHECK(summary->files == 1);
    CHECK_FALSE(summary->stopped);

    seen.clear();
    summary = walkTree(dir.path(), [&](const TreeEntry& entry) {
        seen.push_back(entry.relativePath);
        return seen.size() == 2 ? WalkAction::Stop : WalkAction::Continue;
    });
    REQUIRE(summary.has_value());
    CHECK(seen == std::vector<std::string>{"a", "a/1.txt"});
    CHECK(summary->stopped);

    // A cancellation flag raised by another party (here the visitor) ends the walk before the next entry.
    std::atomic<bool> cancel{false};
    rv::core::TreeWalkOptions options;
    options.cancel = &cancel;
    seen.clear();
    summary = walkTree(
        dir.path(),
        [&](const TreeEntry& entry) {
            seen.push_back(entry.relativePath);
            cancel = seen.size() == 2;
            return WalkAction::Continue;
        },
        options);
    REQUIRE(summary.has_value());
    CHECK(seen.size() == 2);
    CHECK(summary->cancelled);
}

TEST_CASE("core/tree_walk: paths longer than MAX_PATH", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    const std::string level(60, 'd');
    std::string relative;
    for (int i = 0; i < 5; ++i) {
        relative += level + std::to_string(i) + "/";
    }
    relative += "Kodomo Chousadan Mighty Pockets - Chousa 3 - Kyakusen Queen Patra no Nazo (Japan) (SoundLink) [b].bs";
    const fs::path deep = dir.path() / rv::core::pathFromUtf8(relative);
    REQUIRE(deep.native().size() > 300);
    write(deep, "satellaview");

    const auto entries = walked(dir.path());
    const TreeEntry& file = find(entries, relative, TreeEntryKind::File);
    CHECK(file.size == 11);
    for (const TreeEntry& entry : entries) {
        CHECK(entry.kind != TreeEntryKind::Error);
    }
    // The shared reader reaches it too.
    const auto content = rv::core::readFileShared(deep, 1024);
    REQUIRE(content.has_value());
    CHECK(*content == "satellaview");
    rv::test::removeTreeAnyLength(dir.path() / (level + "0"));
}

TEST_CASE("core/tree_walk: junctions are followed, a loop is reported and not entered", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    rv::test::TempDir outside("rv-walk-target");
    write(dir.file("roms/a.gb"), "a");
    write(outside.file("more/b.gb"), "bb");
    auto toOutside = rv::test::Junction::create(dir.file("roms/linked"), outside.path());
    REQUIRE(toOutside.has_value());
    auto loop = rv::test::Junction::create(dir.file("roms/again"), dir.path());
    REQUIRE(loop.has_value());

    const auto entries = walked(dir.path());
    CHECK(outline(entries) == std::vector<std::string>{"D roms", "F roms/a.gb", "D roms/again", "E roms/again",
                                                       "D roms/linked", "D roms/linked/more",
                                                       "F roms/linked/more/b.gb"});
    CHECK(find(entries, "roms/again", TreeEntryKind::Directory).reparseTag == IO_REPARSE_TAG_MOUNT_POINT);
    const TreeEntry& loopError = find(entries, "roms/again", TreeEntryKind::Error);
    CHECK(loopError.error.code == rv::ErrorCode::Unsupported);
    CHECK(loopError.error.message.find("loop") != std::string::npos);
    CHECK(find(entries, "roms/linked/more/b.gb", TreeEntryKind::File).size == 2);

    // A loop through two links: each leads to the other's parent.
    auto back = rv::test::Junction::create(outside.file("more/back"), dir.file("roms"));
    REQUIRE(back.has_value());
    const auto twice = walked(dir.path());
    CHECK(find(twice, "roms/linked/more/back", TreeEntryKind::Error).error.code == rv::ErrorCode::Unsupported);

    // Links not followed: listed, never entered, no error.
    rv::core::TreeWalkOptions options;
    options.followLinks = false;
    CHECK(outline(walked(dir.path(), options)) ==
          std::vector<std::string>{"D roms", "F roms/a.gb", "D roms/again", "D roms/linked"});
}

TEST_CASE("core/tree_walk: a folder that cannot be listed is reported and skipped", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    write(dir.file("a.txt"), "a");
    write(dir.file("denied/hidden from view.txt"), "x");
    write(dir.file("z/z.txt"), "z");
    {
        auto denied = rv::test::ListingDenied::apply(dir.file("denied"));
        REQUIRE(denied.has_value());
        const auto entries = walked(dir.path());
        CHECK(outline(entries) ==
              std::vector<std::string>{"F a.txt", "D denied", "E denied", "D z", "F z/z.txt"});
        // Its content is unknown, not gone: unavailable, the reason kept.
        const TreeEntry& error = find(entries, "denied", TreeEntryKind::Error);
        CHECK(error.error.code == rv::ErrorCode::Unavailable);
        CHECK(error.error.message.find("error 5") != std::string::npos); // ERROR_ACCESS_DENIED

        // As the root, the folder fails the walk.
        const auto root = listTree(dir.file("denied"));
        REQUIRE_FALSE(root.has_value());
        CHECK(root.error().code == rv::ErrorCode::Unavailable);
        CHECK(root.error().message.find("error 5") != std::string::npos);
    }
    CHECK(walked(dir.file("denied")).size() == 1); // permissions restored
}

TEST_CASE("core/tree_walk: System Volume Information cannot be listed", "[core][tree_walk][system]") {
    const fs::path systemVolume(L"C:\\System Volume Information");
    std::error_code ec;
    if (!fs::exists(systemVolume, ec)) {
        SKIP("no C:\\System Volume Information on this machine");
    }
    std::vector<TreeEntry> entries;
    const auto summary = walkTree(fs::path(L"C:\\"), [&](const TreeEntry& entry) {
        entries.push_back(entry);
        // Only that folder is entered: everything else of C:\ is skipped.
        return entry.kind == TreeEntryKind::Directory && entry.relativePath != "System Volume Information"
                   ? WalkAction::SkipChildren
                   : WalkAction::Continue;
    });
    REQUIRE(summary.has_value());
    bool reported = false;
    for (const TreeEntry& entry : entries) {
        reported =
            reported || (entry.kind == TreeEntryKind::Error && entry.relativePath == "System Volume Information");
    }
    if (!reported) {
        SKIP("this account may list System Volume Information");
    }
    CHECK(summary->errors >= 1);
}

TEST_CASE("core/tree_walk: hidden files, leading dots and Unicode names", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    write(dir.file("hidden.gb"), "h");
    REQUIRE(rv::test::setHidden(dir.file("hidden.gb"), true).has_value());
    write(dir.file(".EXE, Cogito Ergo Sum.gb"), ".");
    write(dir.file("Pok\xC3\xA9mon Party Mini (NA).min"), "p");             // é composed (NFC)
    write(dir.file("Pokemon Decomposed e\xCC\x81.min"), "d");                // e + combining acute (NFD)
    write(dir.file("\xE6\x97\xA5\xE6\x9C\xAC.txt"), "j");                    // 日本
    write(dir.file("\xF0\x9F\x8E\xAE.gb"), "e");                             // 🎮
    write(dir.file("Caf\xC3\xA9/Caf\xC3\xA9.zip"), "c");

    const auto entries = walked(dir.path());
    // UTF-8 byte order: "Pokemon" before "Pok\xC3\xA9mon" ('e' is 0x65, the lead byte of é 0xC3), lowercase after
    // uppercase, CJK and emoji last.
    CHECK(outline(entries) == std::vector<std::string>{
                                  "F .EXE, Cogito Ergo Sum.gb",
                                  "D Caf\xC3\xA9",
                                  "F Caf\xC3\xA9/Caf\xC3\xA9.zip",
                                  "F Pokemon Decomposed e\xCC\x81.min",
                                  "F Pok\xC3\xA9mon Party Mini (NA).min",
                                  "F hidden.gb",
                                  "F \xE6\x97\xA5\xE6\x9C\xAC.txt",
                                  "F \xF0\x9F\x8E\xAE.gb",
                              });
    CHECK(find(entries, "hidden.gb", TreeEntryKind::File).hidden);
    CHECK_FALSE(find(entries, ".EXE, Cogito Ergo Sum.gb", TreeEntryKind::File).hidden);
}

TEST_CASE("core/tree_walk: names that Windows would normalize are reopened exactly", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    // Created through \\?\ paths, as other systems and tools leave them on exFAT drives: a plain path would lose the
    // trailing dot or space, or name the device NUL.
    const std::wstring base = rv::core::extendedLengthPath(dir.path());
    const auto create = [&](std::wstring_view relative, std::string_view content) {
        const std::wstring path = base + L"\\" + std::wstring(relative);
        const HANDLE file =
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(file != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        const BOOL done = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(file);
        REQUIRE(done);
    };
    create(L"Game.gb", "plain");
    create(L"Game.gb.", "trailing dot");
    create(L"Game.gb ", "trailing space");
    REQUIRE(CreateDirectoryW((base + L"\\Disc 1.").c_str(), nullptr));
    create(L"Disc 1.\\Track.bin", "track");
    create(L"NUL", "not the device");

    const auto entries = walked(dir.path());
    CHECK(outline(entries) == std::vector<std::string>{"D Disc 1.", "F Disc 1./Track.bin", "F Game.gb", "F Game.gb ",
                                                       "F Game.gb.", "F NUL"});
    const auto read = [&](std::string_view relative) {
        const auto path = rv::core::extendedLengthPathBelow(dir.path(), relative);
        REQUIRE(path.has_value());
        const auto content = rv::core::readFileShared(*path, 64);
        INFO(relative << ": " << (content ? *content : content.error().toString()));
        REQUIRE(content.has_value());
        return *content;
    };
    CHECK(read("Game.gb") == "plain");
    CHECK(read("Game.gb.") == "trailing dot");
    CHECK(read("Game.gb ") == "trailing space");
    CHECK(read("Disc 1./Track.bin") == "track");
    CHECK(read("NUL") == "not the device");
    // What a plain path reaches instead: the sibling, nothing, the device.
    CHECK(rv::core::readFileShared(dir.file("Game.gb."), 64).value() == "plain");
    CHECK(rv::core::readFileShared(dir.file("Disc 1./Track.bin"), 64).error().code == rv::ErrorCode::NotFound);
    CHECK(rv::core::readFileShared(dir.file("NUL"), 64).error().code == rv::ErrorCode::InvalidArgument);

    rv::test::removeTreeAnyLength(dir.path()); // the plain path of TempDir's clean-up would miss these names
    CHECK_FALSE(fs::exists(dir.path()));
}

TEST_CASE("core/tree_walk: a name without a UTF-8 form is reported", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    write(dir.file("a.gb"), "a");
    const fs::path unpaired = dir.path() / std::wstring{L'b', static_cast<wchar_t>(0xD800), L'.', L'g', L'b'};
    write(unpaired, "b");
    write(dir.file("c.gb"), "c");
    const auto entries = walked(dir.path());
    REQUIRE(entries.size() == 3);
    CHECK(outline(entries) == std::vector<std::string>{"F a.gb", "E b\xEF\xBF\xBD.gb", "F c.gb"});
    CHECK(entries[1].error.code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("core/tree_walk: the root's errors", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    write(dir.file("file.txt"), "f");
    const auto missing = listTree(dir.file("missing"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
    const auto notAFolder = listTree(dir.file("file.txt"));
    REQUIRE_FALSE(notAFolder.has_value());
    CHECK(notAFolder.error().code == rv::ErrorCode::InvalidArgument);
    if (const wchar_t letter = unusedDriveLetter(); letter != 0) {
        const auto offline = listTree(fs::path(std::wstring{letter, L':', L'\\', L'r', L'o', L'm', L's'}));
        REQUIRE_FALSE(offline.has_value());
        CHECK(offline.error().code == rv::ErrorCode::Unavailable);
    }
    // An empty root lists nothing.
    fs::create_directories(dir.file("empty"));
    CHECK(walked(dir.file("empty")).empty());
}

TEST_CASE("core/tree_walk: symbolic links to files take their target's size", "[core][tree_walk]") {
    rv::test::TempDir dir("rv-walk");
    write(dir.file("target.bin"), "123456");
    const fs::path link = dir.file("link.bin");
    if (!CreateSymbolicLinkW(link.c_str(), dir.file("target.bin").c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        SKIP("symbolic links need Developer Mode or administrator rights here");
    }
    const auto entries = walked(dir.path());
    const TreeEntry& entry = find(entries, "link.bin", TreeEntryKind::File);
    CHECK(entry.reparseTag == IO_REPARSE_TAG_SYMLINK);
    CHECK(entry.size == 6);
    fs::remove(dir.file("target.bin"));
    CHECK(find(walked(dir.path()), "link.bin", TreeEntryKind::Error).error.code == rv::ErrorCode::NotFound);
}
