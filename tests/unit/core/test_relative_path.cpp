// Gamelist-style paths resolved against the folder they belong to (a system's ROM folder), never leaving it. A path
// outside the folder (OutOfRange) and a malformed one (InvalidArgument) are told apart by their code.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <rv/core/relative_path.hpp>

using rv::core::resolveRelativePath;

namespace {

const std::filesystem::path kBase{L"D:\\Roms\\Emulators\\gb"};

std::string resolved(std::string_view text, const std::filesystem::path& base = kBase) {
    const auto relative = resolveRelativePath(text, base);
    INFO(text << " -> " << (relative ? *relative : relative.error().toString()));
    REQUIRE(relative.has_value());
    return *relative;
}

void refused(rv::ErrorCode code, std::string_view text, std::string_view why,
             const std::filesystem::path& base = kBase) {
    const auto relative = resolveRelativePath(text, base);
    INFO(text << " -> " << (relative ? *relative : relative.error().toString()));
    REQUIRE_FALSE(relative.has_value());
    CHECK(relative.error().code == code);
    CHECK(relative.error().message.find(why) != std::string::npos);
}

void malformed(std::string_view text, std::string_view why, const std::filesystem::path& base = kBase) {
    refused(rv::ErrorCode::InvalidArgument, text, why, base);
}

void outside(std::string_view text, std::string_view why = "outside",
             const std::filesystem::path& base = kBase) {
    refused(rv::ErrorCode::OutOfRange, text, why, base);
}

} // namespace

TEST_CASE("core/relative_path: relative forms", "[core][relative_path]") {
    CHECK(resolved("./a/b.ext") == "a/b.ext");
    CHECK(resolved("a\\b.ext") == "a/b.ext");
    CHECK(resolved(".\\1 GB US - H-Q\\Mole Mania (USA, Europe) (SGB Enhanced).gb") ==
          "1 GB US - H-Q/Mole Mania (USA, Europe) (SGB Enhanced).gb");
    CHECK(resolved("game.gb") == "game.gb");
    CHECK(resolved("./a/./b//c.zip") == "a/b/c.zip");
    CHECK(resolved("./S") == "S");                    // a <folder> entry
    CHECK(resolved("./sub/") == "sub");               // a folder written with its separator
    CHECK(resolved("./Pok\xC3\xA9mon.gb") == "Pok\xC3\xA9mon.gb");
    CHECK(resolved("./100% Star (Europe).cue") == "100% Star (Europe).cue");
    CHECK(resolved("./Galaxian^3 & ~co; [b] {x}.bin") == "Galaxian^3 & ~co; [b] {x}.bin");
    CHECK(resolved("./~/a.gb") == "~/a.gb");          // no home expansion in gamelists
    CHECK(resolved("./.EXE, Cogito.gb") == ".EXE, Cogito.gb");
    CHECK(resolved("./Ultima II - Galactic Disk .atr") == "Ultima II - Galactic Disk .atr");
    // DEL is a valid name character on NTFS and exFAT (they refuse only U+0000 to U+001F).
    CHECK(resolved("./Game\x7FName (World).gb") == "Game\x7FName (World).gb");
}

TEST_CASE("core/relative_path: absolute paths inside the folder", "[core][relative_path]") {
    CHECK(resolved("D:\\Roms\\Emulators\\gb\\a.gb") == "a.gb");
    CHECK(resolved("D:/Roms/Emulators/gb/sub/a.gb") == "sub/a.gb");
    // The folder's part compares as the file system compares names; the rest keeps its case.
    CHECK(resolved("d:\\roms\\emulators\\GB\\Sub\\A.gb") == "Sub/A.gb");
    CHECK(resolved("D:\\Roms\\Emulators\\gb\\.\\x\\\\y.gb") == "x/y.gb");
    const std::filesystem::path accented{L"C:\\Roms\\Pok\u00E9"};
    CHECK(resolved("C:\\ROMS\\POK\xC3\x89\\a.min", accented) == "a.min");
    // A base given with its \\?\ prefix or a trailing separator.
    CHECK(resolved("D:\\Roms\\Emulators\\gb\\a.gb", std::filesystem::path(L"\\\\?\\D:\\Roms\\Emulators\\gb")) ==
          "a.gb");
    CHECK(resolved("D:\\Roms\\Emulators\\gb\\a.gb", std::filesystem::path(L"D:\\Roms\\Emulators\\gb\\")) == "a.gb");
    // A share: absolute paths on it, relative ones below it.
    const std::filesystem::path share{L"\\\\nas\\roms\\gb"};
    CHECK(resolved("\\\\nas\\roms\\gb\\Sub\\a.gb", share) == "Sub/a.gb");
    CHECK(resolved("//NAS/Roms/GB/a.gb", share) == "a.gb");
    CHECK(resolved("./a.gb", share) == "a.gb");
    CHECK(resolved("\\\\nas\\roms\\a.gb", std::filesystem::path(L"\\\\nas\\roms")) == "a.gb");
}

TEST_CASE("core/relative_path: paths outside the folder", "[core][relative_path]") {
    outside("D:\\Roms\\Emulators\\gba\\a.gba"); // a sibling folder
    outside("D:\\Roms\\Emulators\\gbc\\a.gb");  // a name that starts like the folder's
    outside("D:\\Roms\\Emulators\\a.gb");
    outside("D:\\Roms\\Emulators\\gb.\\a.gb"); // compared as written: Windows would drop the dot
    outside("K:\\Roms\\Emulators\\gb\\a.gb");  // another drive
    outside("D:/");
    outside("\\\\server\\share\\gb\\a.gb");    // a share, for a folder on a drive
    outside("//server/share/a.gb");
    outside("\\\\other\\roms\\gb\\a.gb", "outside", std::filesystem::path(L"\\\\nas\\roms\\gb"));
    outside("D:\\Roms\\Emulators\\gb\\a.gb", "outside", std::filesystem::path(L"\\\\nas\\roms\\gb"));
    // ".." that leads out, from the folder or from a drive or share's root.
    outside("./../gba/a.gba", "\"..\" elements lead outside");
    outside("..\\a.gb", "\"..\" elements lead outside");
    outside("./a/../../b.gb", "\"..\" elements lead outside");
    outside("D:\\Roms\\Emulators\\gb\\..\\gba\\a.gba", "\"..\" elements lead outside");
    outside("\\\\nas\\roms\\gb\\..\\..\\..\\x", "\"..\" elements lead outside",
            std::filesystem::path(L"\\\\nas\\roms\\gb"));
}

TEST_CASE("core/relative_path: malformed paths and forms that are not allowed", "[core][relative_path]") {
    malformed("", "empty");
    malformed("./a/../b.gb", "\"..\" elements are not allowed"); // even when it stays inside
    malformed("./../gb/a.gb", "\"..\" elements are not allowed"); // out and back in
    malformed("D:\\Roms\\Emulators\\gb\\..\\gb\\a.gb", "\"..\" elements are not allowed");
    malformed("../a.gb", "\"..\" elements are not allowed", std::filesystem::path(L"C:\\")); // never above a drive
    malformed("D:\\Roms\\Emulators\\gb", "names the folder itself");
    malformed("D:\\Roms\\Emulators\\gb\\", "names the folder itself");
    malformed(".", "names the folder itself");
    malformed("./", "names the folder itself");
    malformed("\\\\nas\\roms\\gb", "names the folder itself", std::filesystem::path(L"\\\\nas\\roms\\gb"));
    malformed("\\\\?\\D:\\Roms\\Emulators\\gb\\a.gb", "forms are not allowed");
    malformed("\\\\.\\D:\\a.gb", "forms are not allowed");
    malformed("\\\\server", "a server and a share");
    malformed("\\\\server\\", "a server and a share");
    malformed("\\\\server\\..\\x", "a server and a share");
    malformed("D:a.gb", "drive-relative");
    malformed("D:", "drive-relative");
    malformed("\\a.gb", "rooted without a drive");
    malformed("/a.gb", "rooted without a drive");
    malformed("./a.gb:stream", "alternate data streams");
    malformed("\\\\ser:ver\\share\\a.gb", "alternate data streams");
    malformed(std::string_view("./a\0.gb", 7), "NUL");
    malformed("./a\t.gb", "control character");
    malformed("./a\x1F.gb", "control character");
    malformed("./Caf\xE9.gb", "malformed UTF-8 at byte offset 5");
}

TEST_CASE("core/relative_path: the base folder must be absolute", "[core][relative_path]") {
    const auto relative = resolveRelativePath("./a.gb", std::filesystem::path("roms/gb"));
    REQUIRE_FALSE(relative.has_value());
    CHECK(relative.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(relative.error().message.find("absolute") != std::string::npos);
}
