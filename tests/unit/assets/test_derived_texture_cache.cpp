// I3 step 5: the derived-texture cache (rv/assets/derived_texture_cache.hpp, ADR-0011 §7): a round trip byte for
// byte, an entry found with its source missing or its media root unplugged, a changed source rebuilt, a corrupt entry
// deleted and rebuilt, trimming to the bound oldest first and never an entry in use, names that never hold the
// source's, and nothing written under the media root. The "media root" here is a temporary folder of generated covers.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>

#include <windows.h>

#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/worker_pool.hpp>

#include "cover_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::assets;
using rv::test::covers::makeCover;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kKind = "covers";

struct Library {
    rv::test::TempDir temp{"rv-tex-cache"};
    fs::path media = temp.path() / "media";
    fs::path cacheRoot = temp.path() / "data" / "cache" / "textures" / "esde";

    /// Writes a generated cover at `relative` under the media root.
    fs::path writeCover(const std::string& relative, std::uint64_t seed, int width = 680, int height = 497) const {
        const fs::path path = media / rv::core::pathFromUtf8(relative);
        REQUIRE(writePng(path, makeCover(width, height, seed)).has_value());
        return path;
    }
};

/// The media root's files and folders, each file with the SHA-256 of its bytes and its size and last write time as
/// its file record holds them (what must never change). A directory listing's copy of those facts may lag behind
/// the file record on NTFS right after a write (core::listedFileFacts), so the record is asked.
std::map<std::string, std::string> listing(const fs::path& root) {
    std::map<std::string, std::string> files;
    if (!fs::exists(root)) {
        return files;
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        std::string facts = "folder";
        if (entry.is_regular_file()) {
            auto reader = rv::core::SharedFileReader::open(entry.path());
            REQUIRE(reader.has_value());
            const auto record = reader->facts().value();
            facts = rv::core::sha256Hex(rv::core::readBinaryFile(entry.path()).value()) + " " +
                    std::to_string(record.size) + " " + std::to_string(record.lastWriteTime);
        }
        files[rv::core::pathToUtf8(entry.path())] = facts;
    }
    return files;
}

std::vector<std::byte> fileBytes(const fs::path& path) {
    auto bytes = rv::core::readBinaryFile(path);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// A drive letter that names no volume on this machine (an unplugged drive), or 0.
wchar_t unusedDriveLetter() {
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; --letter) {
        if ((drives & (1U << (letter - L'A'))) == 0) {
            return letter;
        }
    }
    return 0;
}

} // namespace

TEST_CASE("assets/derived_texture_cache: the folder, the keys and the entry names", "[assets][texture_cache]") {
    const fs::path cache = fs::path("data") / "cache";
    CHECK(textureCacheRoot(cache, "esde").value() == cache / "textures" / "esde");
    CHECK_FALSE(textureCacheRoot(cache, "").has_value());
    CHECK_FALSE(textureCacheRoot(cache, "..").has_value());
    CHECK_FALSE(textureCacheRoot(cache, "a/b").has_value());
    CHECK_FALSE(textureCacheRoot(cache, "a\\b").has_value());

    const CacheKey key = coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, "snes/covers/Comet Courier.png");
    const DerivedTextureCache texture(fs::path("root"));
    const fs::path path = texture.entryPath(key);
    const std::string hex = key.hex();
    CHECK(hex.size() == 64);
    CHECK(path == fs::path("root") / "front" / hex.substr(0, 2) / (hex + ".rvtex"));
    CHECK(rv::core::pathToUtf8(path).find("Comet") == std::string::npos); // never the user's names

    // Every element of the tuple counts.
    CHECK_FALSE(coverCacheKey(TextureClass::Front, TextureFormat::Rgba8Srgb, kKind, "snes/covers/Comet Courier.png") ==
                key);
    CHECK_FALSE(coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, "screenshots",
                              "snes/covers/Comet Courier.png") == key);
    CHECK_FALSE(coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, "snes/covers/Comet Courier.jpg") ==
                key);
    CHECK(coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, "snes/covers/Comet Courier.png") == key);
    // The elements are separated: ("ab", "c") is not ("a", "bc").
    const std::string_view one[] = {"ab", "c"};
    const std::string_view two[] = {"a", "bc"};
    CHECK_FALSE(generatedCacheKey(TextureClass::Spine, TextureFormat::Bc1Srgb, one, {}, {}) ==
                generatedCacheKey(TextureClass::Spine, TextureFormat::Bc1Srgb, two, {}, {}));

    // A spine's key follows its title, its colour and its font.
    const auto& fonts = rv::test::covers::bundledFonts();
    const LabelFonts labelFonts{&fonts.title, &fonts.detail};
    const LabelFonts otherFont{&fonts.detail, &fonts.detail};
    const SpineRequest spine{"Comet Courier", "", Rgb8{1, 2, 3}, 0};
    const CacheKey spineKey = spineCacheKey(spine, TextureFormat::Bc1Srgb, labelFonts);
    CHECK(spineKey.textureClass == TextureClass::Spine);
    CHECK_FALSE(spineCacheKey({"Comet Courier", "", Rgb8{1, 2, 4}, 0}, TextureFormat::Bc1Srgb, labelFonts) == spineKey);
    CHECK_FALSE(spineCacheKey({"Comet Courier!", "", Rgb8{1, 2, 3}, 0}, TextureFormat::Bc1Srgb, labelFonts) ==
                spineKey);
    CHECK_FALSE(spineCacheKey(spine, TextureFormat::Bc1Srgb, otherFont) == spineKey);
    CHECK(spineCacheKey({"Comet Courier", "", Rgb8{1, 2, 3}, 99}, TextureFormat::Bc1Srgb, labelFonts) == spineKey);
}

TEST_CASE("assets/derived_texture_cache: a cover round-trips through the cache byte for byte",
          "[assets][texture_cache]") {
    const Library library;
    const fs::path source = library.writeCover("snes/covers/Comet Courier.png", 1);
    const auto before = listing(library.media);
    const CacheKey key = coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, "snes/covers/Comet Courier.png");

    DerivedTextureCache first(library.cacheRoot);
    auto built = serveCover(first, TextureFormat::Bc1Srgb, kKind, "snes/covers/Comet Courier.png", source);
    REQUIRE(built.has_value());
    CHECK(built->outcome == ServeOutcome::Built);
    REQUIRE(built->texture.source.has_value());
    CHECK(built->texture.source->size == fs::file_size(source));
    CHECK(first.stats().misses == 1);
    CHECK(first.stats().stored == 1);
    const fs::path entry = first.entryPath(key);
    REQUIRE(fs::is_regular_file(entry));
    CHECK(fileBytes(entry) == serializeRvtex(built->texture, key.digest).value());

    // A second run reads the entry without decoding the cover: the same bytes.
    DerivedTextureCache second(library.cacheRoot);
    auto served = serveCover(second, TextureFormat::Bc1Srgb, kKind, "snes/covers/Comet Courier.png", source);
    REQUIRE(served.has_value());
    CHECK(served->outcome == ServeOutcome::CacheHit);
    CHECK(serializeRvtex(served->texture, key.digest).value() == serializeRvtex(built->texture, key.digest).value());
    CHECK(second.stats().hits == 1);
    CHECK(second.stats().stored == 0);

    // The RGBA8 fallback has entries of its own.
    auto fallback = serveCover(second, TextureFormat::Rgba8Srgb, kKind, "snes/covers/Comet Courier.png", source);
    REQUIRE(fallback.has_value());
    CHECK(fallback->outcome == ServeOutcome::Built);
    CHECK(fallback->texture.width == 128);

    CHECK(listing(library.media) == before); // nothing written under the media root
}

TEST_CASE("assets/derived_texture_cache: an entry is served while its source cannot be read",
          "[assets][texture_cache]") {
    const Library library;
    const fs::path source = library.writeCover("snes/covers/Harbor Lights.png", 2);
    {
        DerivedTextureCache cache(library.cacheRoot);
        REQUIRE(serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Harbor Lights.png", source).has_value());
    }

    SECTION("the media root renamed (a drive gone, the folder moved)") {
        fs::rename(library.media, library.temp.path() / "media-away");
        DerivedTextureCache cache(library.cacheRoot);
        auto served = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Harbor Lights.png", source);
        REQUIRE(served.has_value());
        CHECK(served->outcome == ServeOutcome::CacheHitSourceUnavailable);
        CHECK(cache.stats().hitsWithoutSource == 1);
        CHECK_FALSE(fs::exists(library.media)); // nothing recreated there
        // A cover the cache never held: the source's error, for the caller's generated label.
        auto missing = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Other.png",
                                  library.media / "snes" / "covers" / "Other.png");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == rv::ErrorCode::NotFound);
    }
    SECTION("the media root on an unplugged drive") {
        const wchar_t letter = unusedDriveLetter();
        if (letter == 0) {
            SKIP("every drive letter is in use");
        }
        // The same media-relative path, now under a drive that does not answer: the entry is found by its key alone.
        const fs::path unplugged = fs::path(std::wstring(1, letter) + L":\\Media\\snes\\covers\\Harbor Lights.png");
        DerivedTextureCache cache(library.cacheRoot);
        auto served = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Harbor Lights.png", unplugged);
        REQUIRE(served.has_value());
        CHECK(served->outcome == ServeOutcome::CacheHitSourceUnavailable);
        auto missing = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Other.png",
                                  fs::path(std::wstring(1, letter) + L":\\Media\\snes\\covers\\Other.png"));
        REQUIRE_FALSE(missing.has_value());
        CHECK((missing.error().code == rv::ErrorCode::Unavailable || missing.error().code == rv::ErrorCode::NotFound));
    }
    SECTION("the cover deleted") {
        fs::remove(source);
        DerivedTextureCache cache(library.cacheRoot);
        auto served = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Harbor Lights.png", source);
        REQUIRE(served.has_value());
        CHECK(served->outcome == ServeOutcome::CacheHitSourceUnavailable);
    }
}

TEST_CASE("assets/derived_texture_cache: a changed source is decoded again", "[assets][texture_cache]") {
    const Library library;
    const fs::path source = library.writeCover("snes/covers/Lanterns.png", 3);
    DerivedTextureCache cache(library.cacheRoot);
    auto first = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Lanterns.png", source);
    REQUIRE(first.has_value());

    SECTION("another picture") {
        library.writeCover("snes/covers/Lanterns.png", 4);
        auto again = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Lanterns.png", source);
        REQUIRE(again.has_value());
        CHECK(again->outcome == ServeOutcome::Rebuilt);
        CHECK(cache.stats().stale == 1);
        CHECK_FALSE(again->texture.levels[0].data == first->texture.levels[0].data);
        // The replaced entry now matches the new source.
        DerivedTextureCache next(library.cacheRoot);
        CHECK(serveCover(next, TextureFormat::Bc1Srgb, kKind, "snes/covers/Lanterns.png", source)->outcome ==
              ServeOutcome::CacheHit);
    }
    SECTION("the same size, another modification time") {
        fs::last_write_time(source, fs::last_write_time(source) - std::chrono::hours(24));
        auto again = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Lanterns.png", source);
        REQUIRE(again.has_value());
        CHECK(again->outcome == ServeOutcome::Rebuilt);
    }
    SECTION("not an image any more: the error, and the old entry stays for an unplugged root") {
        REQUIRE(rv::core::writeTextFileAtomic(source, "broken").has_value());
        auto broken = serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Lanterns.png", source);
        REQUIRE_FALSE(broken.has_value());
        CHECK(broken.error().code == rv::ErrorCode::Unsupported);
        const CacheKey key = coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, "snes/covers/Lanterns.png");
        CHECK(fs::exists(cache.entryPath(key))); // a stale entry is replaced, never deleted, until a new one exists
    }
}

TEST_CASE("assets/derived_texture_cache: a corrupt entry is deleted and rebuilt", "[assets][texture_cache]") {
    const Library library;
    const fs::path source = library.writeCover("snes/covers/Moonbase.png", 5);
    const CacheKey key = coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, "snes/covers/Moonbase.png");
    DerivedTextureCache cache(library.cacheRoot);
    REQUIRE(serveCover(cache, TextureFormat::Bc1Srgb, kKind, "snes/covers/Moonbase.png", source).has_value());
    const fs::path entry = cache.entryPath(key);
    const std::vector<std::byte> good = fileBytes(entry);

    SECTION("a flipped byte") {
        std::vector<std::byte> bad = good;
        bad[kRvtexHeaderBytes + 100] ^= std::byte{0x10};
        REQUIRE(rv::core::writeFileAtomic(entry, bad).has_value());
    }
    SECTION("truncated") {
        REQUIRE(rv::core::writeFileAtomic(entry, std::span<const std::byte>(good).first(1000)).has_value());
    }
    SECTION("another entry's file under this name") {
        const CacheKey other = coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, "snes/covers/X.png");
        RvTexture texture = parseRvtex(good, nullptr, "entry").value();
        REQUIRE(rv::core::writeFileAtomic(entry, serializeRvtex(texture, other.digest).value()).has_value());
    }
    DerivedTextureCache next(library.cacheRoot);
    auto served = serveCover(next, TextureFormat::Bc1Srgb, kKind, "snes/covers/Moonbase.png", source);
    REQUIRE(served.has_value());
    CHECK(served->outcome == ServeOutcome::Rebuilt);
    CHECK(next.stats().corrupt == 1);
    CHECK(fileBytes(entry) == good); // rebuilt the same, deterministically

    // Without its source, a corrupt entry is never served: deleted, and the source's error returned.
    std::vector<std::byte> bad = good;
    bad[50] ^= std::byte{0x01};
    REQUIRE(rv::core::writeFileAtomic(entry, bad).has_value());
    fs::remove(source);
    DerivedTextureCache last(library.cacheRoot);
    auto gone = serveCover(last, TextureFormat::Bc1Srgb, kKind, "snes/covers/Moonbase.png", source);
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().code == rv::ErrorCode::NotFound);
    CHECK_FALSE(fs::exists(entry));
}

TEST_CASE("assets/derived_texture_cache: trimming removes the oldest entries, never one in use",
          "[assets][texture_cache]") {
    const Library library;
    DerivedTextureCache cache(library.cacheRoot);
    const auto& fonts = rv::test::covers::bundledFonts();
    const LabelFonts labelFonts{&fonts.title, &fonts.detail};
    // Ten spines, each 7,080 bytes on disk, aged one hour apart: spine 0 the oldest.
    std::vector<CacheKey> keys;
    const auto now = fs::file_time_type::clock::now();
    for (int i = 0; i < 10; ++i) {
        const SpineRequest request{"Title " + std::to_string(i), "", Rgb8{10, 20, 30}, 0};
        auto served = serveSpine(cache, labelFonts, request, TextureFormat::Bc1Srgb);
        REQUIRE(served.has_value());
        CHECK(served->outcome == ServeOutcome::Built);
        keys.push_back(spineCacheKey(request, TextureFormat::Bc1Srgb, labelFonts));
        fs::last_write_time(cache.entryPath(keys.back()), now - std::chrono::hours(20 - i));
    }
    const std::uint64_t entryBytes = kRvtexHeaderBytes + layerBytes(TextureClass::Spine, TextureFormat::Bc1Srgb);
    // Leftovers of interrupted writes: an old one removed, a recent one (a write in progress) kept.
    const fs::path oldTemporary = cache.entryPath(keys[0]).string() + ".tmp-123-1";
    const fs::path newTemporary = cache.entryPath(keys[1]).string() + ".tmp-123-2";
    REQUIRE(rv::core::writeTextFileAtomic(oldTemporary, "partial").has_value());
    REQUIRE(rv::core::writeTextFileAtomic(newTemporary, "partial").has_value());
    fs::last_write_time(oldTemporary, now - std::chrono::hours(2));
    // A file that is no entry is left alone.
    REQUIRE(rv::core::writeTextFileAtomic(library.cacheRoot / "spine" / "notes.txt", "not ours").has_value());

    // Room for 6 entries; entry 1 is in use (the shown plan), so 0, 2, 3 and 4 go.
    const std::unordered_set<std::string> inUse = {keys[1].hex()};
    const TrimReport report = cache.trim(6 * entryBytes, inUse);
    CHECK(report.entries == 10);
    CHECK(report.bytesBefore == 10 * entryBytes);
    CHECK(report.removed == 4);
    CHECK(report.keptInUse == 1);
    CHECK(report.bytesAfter == 6 * entryBytes);
    CHECK(report.temporaryFilesRemoved == 1);
    CHECK(report.failures == 0);
    for (int i = 0; i < 10; ++i) {
        INFO("entry " << i);
        CHECK(fs::exists(cache.entryPath(keys[static_cast<std::size_t>(i)])) == (i == 1 || i >= 5));
    }
    CHECK_FALSE(fs::exists(oldTemporary));
    CHECK(fs::exists(newTemporary));
    CHECK(fs::exists(library.cacheRoot / "spine" / "notes.txt"));

    // Under the bound, nothing goes.
    CHECK(cache.trim(kDefaultTextureCacheBytes, {}).removed == 0);
    // Everything not in use goes at a bound of 0.
    const TrimReport all = cache.trim(0, inUse);
    CHECK(all.removed == 5);
    CHECK(fs::exists(cache.entryPath(keys[1])));
}

TEST_CASE("assets/derived_texture_cache: serving an entry renews its time once per run", "[assets][texture_cache]") {
    const Library library;
    const auto& fonts = rv::test::covers::bundledFonts();
    const LabelFonts labelFonts{&fonts.title, &fonts.detail};
    const SpineRequest request{"Comet Courier", "", std::nullopt, 7};
    const CacheKey key = spineCacheKey(request, TextureFormat::Bc1Srgb, labelFonts);
    {
        DerivedTextureCache cache(library.cacheRoot);
        REQUIRE(serveSpine(cache, labelFonts, request, TextureFormat::Bc1Srgb)->outcome == ServeOutcome::Built);
    }
    const fs::path entry = DerivedTextureCache(library.cacheRoot).entryPath(key);
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(100);
    fs::last_write_time(entry, old);

    DerivedTextureCache run(library.cacheRoot);
    REQUIRE(serveSpine(run, labelFonts, request, TextureFormat::Bc1Srgb)->outcome == ServeOutcome::CacheHit);
    CHECK(fs::last_write_time(entry) > old + std::chrono::hours(99)); // renewed on its first serve
    fs::last_write_time(entry, old);
    REQUIRE(serveSpine(run, labelFonts, request, TextureFormat::Bc1Srgb)->outcome == ServeOutcome::CacheHit);
    CHECK(fs::last_write_time(entry) == old); // not again in the same run
    CHECK(run.stats().hits == 2);

    // Generated front labels are cached the same way.
    const FrontLabelRequest label{"Comet Courier", "", "Fantasy Home System", "1994", 7};
    CHECK(serveFrontLabel(run, labelFonts, label, TextureFormat::Bc1Srgb)->outcome == ServeOutcome::Built);
    CHECK(serveFrontLabel(run, labelFonts, label, TextureFormat::Bc1Srgb)->outcome == ServeOutcome::CacheHit);
}

TEST_CASE("assets/derived_texture_cache: workers share one cache", "[assets][texture_cache]") {
    const Library library;
    std::vector<fs::path> sources;
    for (int i = 0; i < 12; ++i) {
        sources.push_back(library.writeCover("snes/covers/Game " + std::to_string(i) + ".png",
                                             static_cast<std::uint64_t>(i), 340, 248));
    }
    DerivedTextureCache cache(library.cacheRoot);
    rv::core::WorkerPool pool(rv::core::WorkerPoolOptions{.threads = 4});
    std::atomic<int> served{0};
    std::atomic<int> failed{0};
    // Each cover twice, at once: both callers get the layer; the last write wins, whole.
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 12; ++i) {
            REQUIRE(pool.submit([&, i](const rv::core::JobContext&) {
                            auto layer = serveCover(cache, TextureFormat::Bc1Srgb, kKind,
                                                    "snes/covers/Game " + std::to_string(i) + ".png",
                                                    sources[static_cast<std::size_t>(i)]);
                            (layer ? served : failed)++;
                        }).has_value());
        }
    }
    REQUIRE(pool.waitIdle(std::chrono::seconds(60)));
    CHECK(served == 24);
    CHECK(failed == 0);
    DerivedTextureCache next(library.cacheRoot);
    for (int i = 0; i < 12; ++i) {
        CHECK(serveCover(next, TextureFormat::Bc1Srgb, kKind, "snes/covers/Game " + std::to_string(i) + ".png",
                         sources[static_cast<std::size_t>(i)])
                  ->outcome == ServeOutcome::CacheHit);
    }
}

TEST_CASE("assets/derived_texture_cache: a label's shape names its front entries (I4 step 2)", "[assets][texture_cache]") {
    // ADR-0013 §4: the front keys gain the label's shape, in lowest terms, but for the layer's own (the 16-bit
    // cartridges' entries keep their names; kCoverPipelineVersion is not raised).
    const std::string_view path = "psx/covers/Comet Courier.png";
    const CacheKey layer = coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, path);
    CHECK(coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, path, LabelAspect{}) == layer);
    const CacheKey square = coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, path, {120, 120});
    CHECK_FALSE(square == layer);
    CHECK(coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, path, {1, 1}) == square);
    CHECK_FALSE(coverCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, kKind, path, {123, 178}) == square);

    const auto& fonts = rv::test::covers::bundledFonts();
    const LabelFonts labelFonts{&fonts.title, &fonts.detail};
    FrontLabelRequest request{"Comet Courier", "", "Fantasy Home System", "1994", 3};
    const CacheKey label = frontLabelCacheKey(request, TextureFormat::Bc1Srgb, labelFonts);
    // The layer's key is I3's: the texts, the colour and the two fonts.
    const std::string_view texts[] = {"Comet Courier", "", "Fantasy Home System", "1994"};
    const std::string_view shas[] = {fonts.title.sha256(), fonts.detail.sha256()};
    CHECK(label == generatedCacheKey(TextureClass::Front, TextureFormat::Bc1Srgb, texts, paletteColour(3), shas));
    request.label = {120, 120};
    const CacheKey squareLabel = frontLabelCacheKey(request, TextureFormat::Bc1Srgb, labelFonts);
    CHECK_FALSE(squareLabel == label);
    request.label = {5, 5};
    CHECK(frontLabelCacheKey(request, TextureFormat::Bc1Srgb, labelFonts) == squareLabel);

    // serveCover() stores and finds the entry under the shape's name.
    const Library library;
    const fs::path source = library.writeCover(std::string(path), 2, 600, 600);
    DerivedTextureCache cache(library.cacheRoot);
    FitOptions options;
    options.label = {120, 120};
    auto served = serveCover(cache, TextureFormat::Bc1Srgb, kKind, path, source, options);
    REQUIRE(served.has_value());
    CHECK(served->outcome == ServeOutcome::Built);
    CHECK(fs::exists(cache.entryPath(square)));
    CHECK_FALSE(fs::exists(cache.entryPath(layer)));
    auto again = serveCover(cache, TextureFormat::Bc1Srgb, kKind, path, source, options);
    REQUIRE(again.has_value());
    CHECK(again->outcome == ServeOutcome::CacheHit);
}
