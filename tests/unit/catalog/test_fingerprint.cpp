// The quick hash (I2 step 11b, rv/catalog/fingerprint.hpp): its golden vectors, computed by an independent oracle
// (scripts/quick_hash_oracle.py, Python's hashlib; `python -I scripts/quick_hash_oracle.py --check
// tests/unit/catalog/test_fingerprint.cpp` must pass), on generated contents and on files; what a file reads (its two
// ends, never more than 128 KiB, never written), names ending with a dot or a space, paths past MAX_PATH, a file
// another program holds open for writing, the errors (missing, a folder, denied, replaced or written while read); a
// hash current or stale for file facts.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <windows.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/fingerprint.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/core/utf8.hpp>

#include "support/file_system.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using namespace rv::catalog;

namespace {

/// A golden vector: the size of a generated content and its quick hash.
struct Golden {
    std::uint64_t size;
    std::string_view digest;
};

// The oracle's table, pasted as it prints it (python -I scripts/quick_hash_oracle.py).
constexpr std::array<Golden, 11> kGolden{{
    // clang-format off
{0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
{1, "0a2c6ea0370d1d49c411a6e2396695fcd4eab03d96e9e7a8a3ec1ec312d9ab38"},
{3, "0aba742d4084cca4afe6fa649e1d379157676d8ed402f22850393b9d1e81fa08"},
{65535, "ec61c2457d0cdc96cf3a8beea1e4ba7b3b1f994d9ae2f6cd6aadeab7db198265"},
{65536, "cf80ab6b3d419aa21d8d75a53728686b09c441293f9fcbf95a81569c6bf8c372"},
{65537, "77b3302381dac5b047699ea0282fba2b9d4d035385361ae1c25a45bb827f7463"},
{131071, "794efc17acf7bfb54bf5b7bcef5be6f4ce1d50eb3e2623a7933ef9f090b4a55e"},
{131072, "3de6b05678494f3163a0e217b9a04ed66719bf0521cd3bb0b0e4009dfd868f21"},
{131073, "bc6fbc6259fd9c938de990eff27b273a4d654fec3cef0b2c8e20494e1d51c764"},
{196608, "b5ed4b105fb248c8af0ad90b86228468e11e32f4913883e951336e39e65d306a"},
{1048583, "0e62f03a1bb9dd7a4f2a222f76475040894898842a04706ee59f300fa14f1144"},
    // clang-format on
}};

/// The oracle's generated content of `size` bytes: a linear congruential generator from 0x52564648, stepped before
/// each byte, its byte (state >> 16) & 0xFF.
std::string content(std::uint64_t size) {
    std::uint32_t state = 0x52564648u;
    std::string out(static_cast<std::size_t>(size), '\0');
    for (char& byte : out) {
        state = state * 1103515245u + 12345u;
        byte = static_cast<char>((state >> 16) & 0xFFu);
    }
    return out;
}

std::span<const std::byte> bytesOf(const std::string& text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

void writeFile(const fs::path& path, const std::string& text) {
    REQUIRE(rv::test::writeFileAnyLength(path, text).has_value());
}

/// Creates `relativeUtf8` ('/' between the elements) below `root` with `text`, every element exactly as written
/// (through
/// \\?\ paths: a trailing dot or space kept), the folders above it included.
void writeExact(const fs::path& root, std::string_view relativeUtf8, const std::string& text) {
    std::wstring path = rv::core::extendedLengthPath(root);
    std::string_view rest = relativeUtf8;
    while (true) {
        const std::size_t slash = rest.find('/');
        path += L"\\" + rv::core::utf8ToWide(rest.substr(0, slash)).value();
        if (slash == std::string_view::npos) {
            break;
        }
        REQUIRE((CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS));
        rest.remove_prefix(slash + 1);
    }
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    const BOOL done = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    REQUIRE(done);
}

std::int64_t timeOf(const fs::path& path) {
    const auto time = rv::core::lastWriteTime(path);
    REQUIRE(time.has_value());
    return fileTimeTicks(*time);
}

} // namespace

TEST_CASE("catalog/fingerprint: the quick hash's golden vectors equal the oracle's", "[catalog][fingerprint]") {
    for (const Golden& golden : kGolden) {
        INFO(golden.size);
        const std::string data = content(golden.size);
        CHECK(toHex(quickHashDigest(bytesOf(data))) == golden.digest);
        CHECK(quickHashBytes(golden.size) == std::min<std::uint64_t>(golden.size, kQuickHashWholeFileLimit));
    }
    // The empty file hashes to SHA-256 of nothing; a file up to 128 KiB is hashed whole, each byte once.
    CHECK(toHex(quickHashDigest({})) == kGolden[0].digest);
    const std::string whole = content(kQuickHashWholeFileLimit);
    CHECK(quickHashDigest(bytesOf(whole)) == rv::core::sha256(bytesOf(whole)));
    // Beyond it, only the two ends count: another middle, the same hash; another end, another hash.
    std::string large = content(300'000);
    const rv::core::Sha256Digest original = quickHashDigest(bytesOf(large));
    large[150'000] = static_cast<char>(large[150'000] ^ 0x5A);
    CHECK(quickHashDigest(bytesOf(large)) == original);
    large[large.size() - 1] = static_cast<char>(large[large.size() - 1] ^ 0x5A);
    CHECK(quickHashDigest(bytesOf(large)) != original);
    large = content(300'000);
    large[kQuickHashPartBytes - 1] = static_cast<char>(large[kQuickHashPartBytes - 1] ^ 0x5A);
    CHECK(quickHashDigest(bytesOf(large)) != original);
}

TEST_CASE("catalog/fingerprint: a file's quick hash is read with its size and time, the oracle's digest",
          "[catalog][fingerprint]") {
    const rv::test::TempDir dir("rv-quick-hash");
    for (const Golden& golden : kGolden) {
        INFO(golden.size);
        const fs::path file = dir.file("files/" + std::to_string(golden.size) + ".bin");
        writeFile(file, content(golden.size));
        const auto hash = readQuickHashBelow(dir.file("files"), std::to_string(golden.size) + ".bin");
        INFO((hash ? std::string("read") : hash.error().toString()));
        REQUIRE(hash.has_value());
        CHECK(toHex(hash->digest) == golden.digest);
        CHECK(hash->size == golden.size);
        CHECK(hash->lastWriteTime == timeOf(file));
        CHECK(isCurrent(*hash, FileFacts{false, golden.size, timeOf(file), false}));
    }
    // The facts a hash is stored with are those a listing gives, which the import records.
    const auto listed = rv::core::listTree(dir.file("files"));
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == kGolden.size());
    for (const rv::core::TreeEntry& entry : *listed) {
        INFO(entry.relativePath);
        const auto hash = readQuickHashBelow(dir.file("files"), entry.relativePath);
        REQUIRE(hash.has_value());
        CHECK(isCurrent(*hash, FileFacts{false, entry.size, fileTimeTicks(entry.lastWriteTime), false}));
    }
}

TEST_CASE("catalog/fingerprint: reading a quick hash writes nothing and reaches every name the file system holds",
          "[catalog][fingerprint]") {
    const rv::test::TempDir dir("rv-quick-hash-names");
    const std::string data = content(200'000);
    const std::string expected = toHex(quickHashDigest(bytesOf(data)));

    // Names ending with a dot or a space, which a plain path loses, and a path past MAX_PATH.
    const std::string deep = std::string(120, 'd') + "/" + std::string(120, 'e') + "/" + std::string(60, 'f');
    writeExact(dir.path(), "Game.gb", "the sibling a plain path would reach");
    for (const std::string& name : {std::string("Game.gb."), std::string("Disc 1 /Track.bin "), deep + "/Long.gb"}) {
        INFO(name);
        writeExact(dir.path(), name, data);
        const auto hash = readQuickHashBelow(dir.path(), name);
        INFO((hash ? std::string("read") : hash.error().toString()));
        REQUIRE(hash.has_value());
        CHECK(toHex(hash->digest) == expected);
    }
    CHECK(rv::core::pathToUtf8(dir.path()).size() + deep.size() > 260);

    // Never written: the content and the last write time are as they were.
    const fs::path file = dir.file("Kept.gb");
    writeFile(file, data);
    const std::int64_t before = timeOf(file);
    REQUIRE(readQuickHash(file).has_value());
    CHECK(timeOf(file) == before);
    CHECK(rv::core::readBinaryFile(file).value().size() == data.size());

    // A file another program holds open for writing (sharing reads, as ES-DE's own writers do) is read.
    {
        std::ofstream held(file, std::ios::binary | std::ios::app);
        REQUIRE(held.is_open());
        const auto hash = readQuickHash(file);
        INFO((hash ? std::string("read") : hash.error().toString()));
        REQUIRE(hash.has_value());
        CHECK(toHex(hash->digest) == expected);
    }
}

TEST_CASE("catalog/fingerprint: a file that cannot be read is an error, never a hash", "[catalog][fingerprint]") {
    const rv::test::TempDir dir("rv-quick-hash-errors");
    const auto missing = readQuickHashBelow(dir.path(), "Missing.gb");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);

    std::filesystem::create_directories(dir.file("Folder.gb"));
    const auto folder = readQuickHashBelow(dir.path(), "Folder.gb");
    REQUIRE_FALSE(folder.has_value());
    CHECK(folder.error().code == rv::ErrorCode::InvalidArgument);

    const auto escape = readQuickHashBelow(dir.path(), "../outside.gb");
    REQUIRE_FALSE(escape.has_value());
    CHECK(escape.error().code == rv::ErrorCode::InvalidArgument);

    // Its data denied to this user: the file is listed, its attributes readable, its bytes not.
    const fs::path denied = dir.file("Denied.gb");
    writeFile(denied, content(1000));
    {
        auto guard = rv::test::DataReadDenied::apply(denied);
        REQUIRE(guard.has_value());
        const auto refused = readQuickHash(denied);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code != rv::ErrorCode::NotFound);
    }
    CHECK(readQuickHash(denied).has_value());
}

TEST_CASE("catalog/fingerprint: a file replaced or written while its quick hash is read is an error, never a hash "
          "stored with another file's facts",
          "[catalog][fingerprint]") {
    const rv::test::TempDir dir("rv-quick-hash-races");
    const fs::path file = dir.file("Game.gb");
    const std::string data = content(200'000);
    using detail::QuickHashReadStage;
    const auto readWith = [&](QuickHashReadStage at, const std::function<void()>& change) {
        writeFile(file, data);
        return detail::readQuickHashObserved(file, [&](QuickHashReadStage stage) {
            if (stage == at) {
                change();
            }
        });
    };
    // Replaced whole (another file renamed over it, as an atomic replace or an extraction does) after the listing,
    // before the open: the handle reads the new file alone, but the facts listed first are the old one's.
    const auto replace = [&] {
        const fs::path temporary = dir.file("Game.gb.new");
        writeFile(temporary, content(150'000));
        REQUIRE(MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING));
    };
    // Written in place through another handle (sharing reads, as the reader shares writes).
    const auto append = [&] {
        std::ofstream writer(file, std::ios::binary | std::ios::app);
        REQUIRE(writer.is_open());
        writer << "more";
    };
    for (const auto& [stage, change, what] :
         {std::tuple{QuickHashReadStage::Listed, std::function<void()>(replace), "replaced before the open"},
          std::tuple{QuickHashReadStage::Listed, std::function<void()>(append), "written before the open"},
          std::tuple{QuickHashReadStage::Opened, std::function<void()>(append), "written before the reads"},
          std::tuple{QuickHashReadStage::Read, std::function<void()>(append), "written after the reads"}}) {
        INFO(what);
        const auto hash = readWith(stage, change);
        REQUIRE_FALSE(hash.has_value());
        CHECK(hash.error().code == rv::ErrorCode::IoError);
    }
    // Left alone, each stage is reached once, in order, and the hash is the file's.
    writeFile(file, data);
    std::vector<QuickHashReadStage> stages;
    const auto hash = detail::readQuickHashObserved(file, [&](QuickHashReadStage stage) { stages.push_back(stage); });
    REQUIRE(hash.has_value());
    CHECK(hash->digest == quickHashDigest(bytesOf(data)));
    CHECK(stages == std::vector<QuickHashReadStage>{QuickHashReadStage::Listed, QuickHashReadStage::Opened,
                                                    QuickHashReadStage::Read});
}

TEST_CASE("catalog/fingerprint: a quick hash is current while the file has its size and time",
          "[catalog][fingerprint]") {
    const QuickHash hash{quickHashDigest({}), 1024, 5000};
    CHECK(isCurrent(hash, FileFacts{false, 1024, 5000, false}));
    CHECK(isCurrent(hash, FileFacts{false, 1024, 5000, true})); // hidden or not
    CHECK_FALSE(isCurrent(hash, FileFacts{false, 1025, 5000, false}));
    CHECK_FALSE(isCurrent(hash, FileFacts{false, 1024, 5001, false}));
    CHECK_FALSE(isCurrent(hash, FileFacts{true, 1024, 5000, false})); // a folder has no quick hash
}
