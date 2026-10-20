#pragma once

// The derived-texture cache (I3 step 5, ADR-0011 §7): the collection's layers kept as .rvtex files (rvtex.hpp) under
// <user data>/cache/textures/<library id>/<class>/<2 hex>/<sha256>.rvtex, so that a second start reads a few tens of
// MiB instead of decoding the covers again, and a room whose media root is unplugged still shows the covers it showed.
//
// - Names: the SHA-256 of a key tuple, never the user's file names. A cover's tuple is (kCacheKeyTag, the pipeline
//   version, the class, the format, "media", the media kind, the media file's path relative to the media root as the
//   catalog derives it); a generated layer's (kCacheKeyTag, the pipeline version, the class, the format, "text", its
//   texts, its base colour, the fonts' SHA-256). Elements are UTF-8, separated by one NUL byte. The format is part of
//   the key, so the RGBA8 fallback never reads a BC1 entry; a new pipeline version names new entries.
// - Invalidation: when the source can be read, its size and last write time are compared with the entry's and a
//   different source is decoded again and its entry replaced; when it cannot (the media root unavailable, the file
//   gone) the entry is served as it is. A file that fails its checks (rvtex.hpp: magic, versions, sizes, checksums, the
//   key) is deleted and rebuilt.
// - Size: the cache is derived and can be deleted at any time. It is kept under kDefaultTextureCacheBytes: an entry's
//   modification time is set when it is first served in a run (Windows keeps no reliable access times), and trim()
//   (at start, on a worker) removes the oldest entries beyond the bound, never one the shown plan uses.
// - Writes are atomic (core::writeFileAtomic). Nothing is ever written outside the cache's root: the sources (under the
//   media root) are only opened for reading, with full sharing (core::SharedFileReader).
//
// A DerivedTextureCache may be used by several worker threads at once.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <rv/assets/collection_labels.hpp>
#include <rv/assets/cover_fit.hpp>
#include <rv/assets/rvtex.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/sha256.hpp>

namespace rv::assets {

inline constexpr std::string_view kCacheKeyTag = "rv-tex/1";
inline constexpr std::uint64_t kDefaultTextureCacheBytes = 512ull * 1024ull * 1024ull;
/// .rvtex files larger than this are not read (the largest layer, RGBA8 front, is under 64 KiB).
inline constexpr std::uintmax_t kMaxRvtexFileBytes = 1024ull * 1024ull;

/// <cacheDir>/textures/<libraryId> (cacheDir: core::Paths::cacheDir()). Errors: InvalidArgument when `libraryId` is not
/// one path element of letters, digits, '-', '_' and '.' (not "." or "..").
[[nodiscard]] Expected<std::filesystem::path> textureCacheRoot(const std::filesystem::path& cacheDir,
                                                               std::string_view libraryId);

struct CacheKey {
    core::Sha256Digest digest{};
    TextureClass textureClass = TextureClass::Front;

    [[nodiscard]] std::string hex() const; ///< the digest in lowercase hex: the entry's file name
    bool operator==(const CacheKey&) const = default;
};

/// A cover's key: `mediaKind` as the catalog names it ("covers"), `mediaRelativePath` its path relative to the media
/// root as the catalog derives it ("snes/covers/Game (USA).png", '/' between the elements), and the label's shape the
/// cover is fitted to (ADR-0013 §4: one more element, "label=<w>x<h>" in lowest terms, only for a shape other than the
/// layer's, so that the 16-bit cartridges' entries keep their names and a warm cache stays warm).
[[nodiscard]] CacheKey coverCacheKey(TextureClass textureClass, TextureFormat format, std::string_view mediaKind,
                                     std::string_view mediaRelativePath, const LabelAspect& label = {});

/// A generated layer's key from its texts, base colour and fonts.
[[nodiscard]] CacheKey generatedCacheKey(TextureClass textureClass, TextureFormat format,
                                         std::span<const std::string_view> texts, Rgb8 colour,
                                         std::span<const std::string_view> fontSha256s);

/// The keys of a spine and a generated front label: every input that changes their pixels (the label's shape as a
/// last element "label=<w>x<h>" for a shape other than the layer's).
[[nodiscard]] CacheKey spineCacheKey(const SpineRequest& request, TextureFormat format, const LabelFonts& fonts);
[[nodiscard]] CacheKey frontLabelCacheKey(const FrontLabelRequest& request, TextureFormat format,
                                          const LabelFonts& fonts);

/// What a lookup found.
enum class LookupOutcome {
    Hit,     ///< the entry, valid, made from the same source (or served without its source)
    Missing, ///< no entry
    Stale,   ///< an entry made from another version of the source (kept until it is replaced)
    Corrupt, ///< an entry that failed its checks: deleted
};

struct Lookup {
    LookupOutcome outcome = LookupOutcome::Missing;
    std::optional<RvTexture> texture; ///< set for Hit
};

/// Counters since the cache object was made (the run report's cache hits and misses, I3 step 12).
struct CacheStats {
    std::uint64_t hits = 0;              ///< entries served (with their source checked, or generated)
    std::uint64_t hitsWithoutSource = 0; ///< entries served as they were, their source unreadable (also in `hits`)
    std::uint64_t misses = 0;            ///< lookups that found no entry
    std::uint64_t stale = 0;             ///< entries replaced because their source changed
    std::uint64_t corrupt = 0;           ///< entries deleted because they failed their checks
    std::uint64_t stored = 0;            ///< entries written
    std::uint64_t storeFailures = 0;     ///< writes that failed (the layer is still served)
};

struct TrimReport {
    std::uint64_t entries = 0;      ///< .rvtex files found
    std::uint64_t bytesBefore = 0;
    std::uint64_t bytesAfter = 0;
    std::uint64_t removed = 0;      ///< entries deleted, oldest first
    std::uint64_t keptInUse = 0;    ///< entries beyond the bound kept because the shown plan uses them
    std::uint64_t temporaryFilesRemoved = 0; ///< leftovers of interrupted writes, older than an hour
    std::uint64_t failures = 0;     ///< files that could not be deleted
};

class DerivedTextureCache {
public:
    /// The cache of one library (textureCacheRoot()); nothing is created until an entry is stored.
    explicit DerivedTextureCache(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return m_root; }
    /// <root>/<class>/<first 2 hex digits>/<hex>.rvtex
    [[nodiscard]] std::filesystem::path entryPath(const CacheKey& key) const;

    /// Reads the entry of `key`. With `source` set, the entry is a hit only when it was made from that source (else
    /// Stale); without it (a generated layer, or `sourceUnavailable`: a cover whose source cannot be read) a valid
    /// entry is a hit as it is. Updates the counters (a hit counts as served: markServed()).
    [[nodiscard]] Lookup lookup(const CacheKey& key, const std::optional<SourceStamp>& source,
                                bool sourceUnavailable = false);

    /// Writes the entry atomically (parent folders created). The entry counts as served in this run.
    [[nodiscard]] Status store(const CacheKey& key, const RvTexture& texture);

    /// Sets the entry's modification time to now the first time it is served in this run (later calls do nothing), so
    /// that trim() removes the entries unused for longest.
    void markServed(const CacheKey& key);

    /// Removes the oldest entries (by modification time, then name) until the cache holds at most `maxBytes`, never
    /// one whose hex name is in `inUse`; also removes temporary files of interrupted writes older than an hour.
    [[nodiscard]] TrimReport trim(std::uint64_t maxBytes, const std::unordered_set<std::string>& inUse);

    [[nodiscard]] CacheStats stats() const noexcept;

private:
    std::filesystem::path m_root;
    std::mutex m_servedMutex;
    std::unordered_set<std::string> m_served; ///< hex names served in this run (never iterated into outputs)
    std::atomic<std::uint64_t> m_hits{0};
    std::atomic<std::uint64_t> m_hitsWithoutSource{0};
    std::atomic<std::uint64_t> m_misses{0};
    std::atomic<std::uint64_t> m_stale{0};
    std::atomic<std::uint64_t> m_corrupt{0};
    std::atomic<std::uint64_t> m_stored{0};
    std::atomic<std::uint64_t> m_storeFailures{0};
};

/// How a layer was served.
enum class ServeOutcome {
    CacheHit,                  ///< from the cache, the source checked (or a generated layer)
    CacheHitSourceUnavailable, ///< from the cache as it was: the source could not be read
    Built,                     ///< made now (no entry), and stored
    Rebuilt,                   ///< made now because the source changed or the entry was corrupt, and stored
};

[[nodiscard]] std::string_view toString(ServeOutcome outcome) noexcept;

struct ServedLayer {
    RvTexture texture;
    ServeOutcome outcome = ServeOutcome::Built;
};

/// A cover's front layer, ADR-0011 §6 and §7: the source opened for reading (full sharing) and its size and time
/// compared with the entry's; a hit is served without reading the file; else the file is read, decoded and fitted
/// (buildCoverLayer(), to FitOptions::label's shape, which names the entry too), stamped, stored and served. When the
/// source cannot be opened (NotFound, Unavailable, Conflict,
/// IoError) a valid entry is served as it is. Errors: the source's when there is no entry to serve, and those of
/// buildCoverLayer() for a file that is not a usable image (the caller then shows the generated label).
/// `mediaRelativePath` goes into the key; `sourcePath` is the file itself (logged at debug level only).
[[nodiscard]] Expected<ServedLayer> serveCover(DerivedTextureCache& cache, TextureFormat format,
                                               std::string_view mediaKind, std::string_view mediaRelativePath,
                                               const std::filesystem::path& sourcePath,
                                               const FitOptions& options = {});

/// A spine and a generated front label from the cache, or drawn, stored and served.
[[nodiscard]] Expected<ServedLayer> serveSpine(DerivedTextureCache& cache, const LabelFonts& fonts,
                                               const SpineRequest& request, TextureFormat format);
[[nodiscard]] Expected<ServedLayer> serveFrontLabel(DerivedTextureCache& cache, const LabelFonts& fonts,
                                                    const FrontLabelRequest& request, TextureFormat format);

} // namespace rv::assets
