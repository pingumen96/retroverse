// Reading files another program may be rewriting (core::readFileShared, ADR-0008 §7), and file I/O on names without a
// UTF-8 form (no exception escapes, code survey risk 7).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include <windows.h>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/utf8.hpp>

#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::core::readFileShared;

namespace {

/// Closes a handle on scope exit.
struct HeldFile {
    HANDLE handle;
    ~HeldFile() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

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

TEST_CASE("core/file_io: a shared read while another program writes the file", "[core][file_io]") {
    rv::test::TempDir dir;
    const fs::path file = dir.file("gamelist.xml");
    REQUIRE(rv::core::writeTextFileAtomic(file, "<gameList/>").has_value());

    // ES-DE keeps its own handle open for writing, sharing reads (pugixml saves through fopen).
    const HeldFile writer{CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(writer.handle != INVALID_HANDLE_VALUE);
    const auto text = readFileShared(file, 1024);
    REQUIRE(text.has_value());
    CHECK(*text == "<gameList/>");
}

TEST_CASE("core/file_io: what a shared read reports", "[core][file_io]") {
    rv::test::TempDir dir;
    const fs::path file = dir.file("data.bin");
    REQUIRE(rv::core::writeTextFileAtomic(file, std::string(2048, 'x')).has_value());

    CHECK(readFileShared(file, 2048).value().size() == 2048);
    CHECK(readFileShared(file, 2047).error().code == rv::ErrorCode::OutOfRange);
    CHECK(readFileShared(dir.file("missing.xml"), 1024).error().code == rv::ErrorCode::NotFound);
    CHECK(readFileShared(dir.file("missing/folder.xml"), 1024).error().code == rv::ErrorCode::NotFound);
    CHECK(readFileShared(dir.path(), 1024).error().code == rv::ErrorCode::InvalidArgument);
    CHECK(readFileShared(fs::path("NUL"), 1024).error().code == rv::ErrorCode::InvalidArgument);
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("empty.xml"), "").has_value());
    CHECK(readFileShared(dir.file("empty.xml"), 1024).value().empty());

    {
        // Opened by another program without sharing: a conflict, worth a retry, not a missing file.
        const HeldFile exclusive{CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(exclusive.handle != INVALID_HANDLE_VALUE);
        const auto locked = readFileShared(file, 4096);
        REQUIRE_FALSE(locked.has_value());
        CHECK(locked.error().code == rv::ErrorCode::Conflict);
        CHECK(locked.error().context == rv::core::pathToUtf8Lossy(file));
    }
    {
        // Shared, but a range of it locked by the other program (LockFileEx): the read fails, a conflict too.
        const HeldFile holder{CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(holder.handle != INVALID_HANDLE_VALUE);
        OVERLAPPED range{};
        REQUIRE(LockFileEx(holder.handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 100, 0, &range));
        const auto locked = readFileShared(file, 4096);
        REQUIRE_FALSE(locked.has_value());
        CHECK(locked.error().code == rv::ErrorCode::Conflict);
        CHECK(locked.error().message.find("error 33") != std::string::npos); // ERROR_LOCK_VIOLATION
        UnlockFileEx(holder.handle, 0, 100, 0, &range);
    }

    if (const wchar_t letter = unusedDriveLetter(); letter != 0) {
        // An unplugged or unmapped drive is unavailable, not missing.
        const fs::path offline(std::wstring{letter, L':', L'\\', L'a', L'.', L'x', L'm', L'l'});
        CHECK(readFileShared(offline, 1024).error().code == rv::ErrorCode::Unavailable);
        CHECK(rv::core::readTextFile(offline).error().code == rv::ErrorCode::Unavailable);
    }
}

TEST_CASE("core/file_io: names without a UTF-8 form never throw", "[core][file_io]") {
    rv::test::TempDir dir;
    // NTFS takes any UTF-16 name, an unpaired surrogate included; UTF-8 cannot hold it.
    const fs::path odd = dir.path() / std::wstring{L'r', L'o', L'm', static_cast<wchar_t>(0xDC00), L'.', L'g', L'b'};
    CHECK_FALSE(rv::core::tryPathToUtf8(odd).has_value());

    const auto missing = rv::core::readTextFile(odd);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
    CHECK(missing.error().context.find("rom\xEF\xBF\xBD.gb") != std::string::npos);
    CHECK(rv::core::readBinaryFile(odd).error().code == rv::ErrorCode::NotFound);
    CHECK(readFileShared(odd, 16).error().code == rv::ErrorCode::NotFound);

    REQUIRE(rv::core::writeTextFileAtomic(odd, "payload").has_value());
    CHECK(rv::core::readTextFile(odd).value() == "payload");
    CHECK(readFileShared(odd, 16).value() == "payload");

    // A JSON file so named reads, and its errors name it with U+FFFD.
    const fs::path json = dir.path() / std::wstring{L'c', L'f', L'g', static_cast<wchar_t>(0xD800), L'.', L'j', L's',
                                                    L'o', L'n'};
    REQUIRE(rv::core::writeTextFileAtomic(json, "{\"a\": 1}").has_value());
    CHECK(rv::core::readJsonFile(json).value()["a"] == 1);
    REQUIRE(rv::core::writeTextFileAtomic(json, "{\"a\": ").has_value());
    const auto broken = rv::core::readJsonFile(json);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().context.find("cfg\xEF\xBF\xBD.json") != std::string::npos);
}

TEST_CASE("core/file_io: the \\\\?\\ form of a path", "[core][file_io]") {
    using rv::core::extendedLengthPath;
    CHECK(extendedLengthPath(fs::path(L"C:\\Roms\\gb\\a.gb")) == L"\\\\?\\C:\\Roms\\gb\\a.gb");
    CHECK(extendedLengthPath(fs::path(L"C:/Roms/./gb/../gb/a.gb")) == L"\\\\?\\C:\\Roms\\gb\\a.gb");
    CHECK(extendedLengthPath(fs::path(L"\\\\nas\\roms\\gb")) == L"\\\\?\\UNC\\nas\\roms\\gb");
    CHECK(extendedLengthPath(fs::path(L"\\\\?\\C:\\x")) == L"\\\\?\\C:\\x");
    CHECK(extendedLengthPath(fs::path(L"\\\\.\\NUL")) == L"\\\\.\\NUL");
    const std::wstring relative = extendedLengthPath(fs::path(L"sub\\file.txt"));
    CHECK(relative.starts_with(L"\\\\?\\"));
    CHECK(relative.ends_with(L"\\sub\\file.txt"));
    // The normalization of a plain path changes names: what a listed name must not go through.
    CHECK(extendedLengthPath(fs::path(L"C:\\Roms\\Disc 1.\\Track.bin ")) == L"\\\\?\\C:\\Roms\\Disc 1\\Track.bin");
    CHECK(extendedLengthPath(fs::path(L"C:\\Roms\\NUL")) == L"\\\\.\\NUL");
}

TEST_CASE("core/file_io: the \\\\?\\ form of a listed name below its root", "[core][file_io]") {
    const auto below = [](const fs::path& root, std::string_view relative) {
        const auto path = rv::core::extendedLengthPathBelow(root, relative);
        INFO(relative << ": " << (path ? rv::core::pathToUtf8Lossy(*path) : path.error().toString()));
        REQUIRE(path.has_value());
        return path->native();
    };
    // The root is normalized, the names are kept exactly as listed.
    CHECK(below(fs::path(L"C:/Roms/./gb/"), "a.gb") == L"\\\\?\\C:\\Roms\\gb\\a.gb");
    CHECK(below(fs::path(L"C:\\Roms"), "Disc 1./Track.bin ") == L"\\\\?\\C:\\Roms\\Disc 1.\\Track.bin ");
    CHECK(below(fs::path(L"C:\\Roms"), "sub\\NUL") == L"\\\\?\\C:\\Roms\\sub\\NUL");
    CHECK(below(fs::path(L"C:\\Roms"), "Pok\xC3\xA9mon.gb") == L"\\\\?\\C:\\Roms\\Pok\u00E9mon.gb");
    CHECK(below(fs::path(L"C:\\"), "a.gb") == L"\\\\?\\C:\\a.gb");
    CHECK(below(fs::path(L"\\\\nas\\roms\\"), "gb/a.gb") == L"\\\\?\\UNC\\nas\\roms\\gb\\a.gb");
    CHECK(below(fs::path(L"\\\\?\\C:\\Roms\\"), "a.gb") == L"\\\\?\\C:\\Roms\\a.gb");

    for (const std::string_view relative :
         {std::string_view(), std::string_view("a//b.gb"), std::string_view("a/"), std::string_view("./a.gb"),
          std::string_view("a/../b.gb"), std::string_view(".."), std::string_view("a.gb:stream"),
          std::string_view("a\0b.gb", 6), std::string_view("Caf\xE9.gb")}) {
        const auto path = rv::core::extendedLengthPathBelow(fs::path(L"C:\\Roms"), relative);
        INFO((rv::core::isValidUtf8(relative) ? std::string(relative) : std::string("(malformed UTF-8)")));
        REQUIRE_FALSE(path.has_value());
        CHECK(path.error().code == rv::ErrorCode::InvalidArgument);
        CHECK(path.error().context == "C:\\Roms");
    }
}
