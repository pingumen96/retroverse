#include <rv/assets/derived_texture_cache.hpp>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

namespace fs = std::filesystem;

namespace rv::assets {
namespace {

constexpr std::string_view kEntryExtension = ".rvtex";
constexpr auto kTemporaryFileAge = std::chrono::hours(1);

class KeyHasher {
public:
    KeyHasher(TextureClass textureClass, TextureFormat format, std::string_view kind) {
        m_hash.update(kCacheKeyTag);
        add(std::to_string(kCoverPipelineVersion));
        add(toString(textureClass));
        add(toString(format));
        add(kind);
    }
    void add(std::string_view part) {
        m_hash.update(std::string_view("\0", 1));
        m_hash.update(part);
    }
    core::Sha256Digest finish() { return m_hash.finish(); }

private:
    core::Sha256 m_hash;
};

bool isHexName(std::string_view name) noexcept {
    return name.size() == 64 && std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

template <class F>
Expected<ServedLayer> serveGenerated(DerivedTextureCache& cache, const CacheKey& key, F&& build) {
    Lookup found = cache.lookup(key, std::nullopt);
    if (found.outcome == LookupOutcome::Hit) {
        return ServedLayer{std::move(*found.texture), ServeOutcome::CacheHit};
    }
    auto built = build();
    if (!built) {
        return propagate(built.error());
    }
    if (auto stored = cache.store(key, built->texture); !stored) {
        spdlog::warn("Derived-texture cache: an entry could not be written ({})", stored.error().message);
    }
    return ServedLayer{std::move(built->texture),
                       found.outcome == LookupOutcome::Missing ? ServeOutcome::Built : ServeOutcome::Rebuilt};
}

} // namespace

Expected<fs::path> textureCacheRoot(const fs::path& cacheDir, std::string_view libraryId) {
    const bool valid = !libraryId.empty() && libraryId != "." && libraryId != ".." && libraryId.size() <= 64 &&
                       std::all_of(libraryId.begin(), libraryId.end(), [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                  c == '-' || c == '_' || c == '.';
                       });
    if (!valid) {
        return makeError(ErrorCode::InvalidArgument, "not a library id the cache can name a folder after",
                         std::string(libraryId));
    }
    return cacheDir / "textures" / fs::path(std::string(libraryId));
}

std::string CacheKey::hex() const {
    return core::toHex(digest);
}

CacheKey coverCacheKey(TextureClass textureClass, TextureFormat format, std::string_view mediaKind,
                       std::string_view mediaRelativePath) {
    KeyHasher hasher(textureClass, format, "media");
    hasher.add(mediaKind);
    hasher.add(mediaRelativePath);
    return {hasher.finish(), textureClass};
}

CacheKey generatedCacheKey(TextureClass textureClass, TextureFormat format, std::span<const std::string_view> texts,
                           Rgb8 colour, std::span<const std::string_view> fontSha256s) {
    KeyHasher hasher(textureClass, format, "text");
    hasher.add(std::to_string(texts.size()));
    for (const std::string_view text : texts) {
        hasher.add(text);
    }
    hasher.add(toHexCode(colour));
    for (const std::string_view sha : fontSha256s) {
        hasher.add(sha);
    }
    return {hasher.finish(), textureClass};
}

CacheKey spineCacheKey(const SpineRequest& request, TextureFormat format, const LabelFonts& fonts) {
    const std::string_view texts[] = {request.title, request.fallbackTitle};
    const std::string_view shas[] = {fonts.title != nullptr ? std::string_view(fonts.title->sha256()) : ""};
    return generatedCacheKey(TextureClass::Spine, format, texts, spineBaseColour(request), shas);
}

CacheKey frontLabelCacheKey(const FrontLabelRequest& request, TextureFormat format, const LabelFonts& fonts) {
    const std::string_view texts[] = {request.title, request.fallbackTitle, request.systemName, request.year};
    const FontFace* detail = fonts.detail != nullptr ? fonts.detail : fonts.title;
    const std::string_view shas[] = {fonts.title != nullptr ? std::string_view(fonts.title->sha256()) : "",
                                     detail != nullptr ? std::string_view(detail->sha256()) : ""};
    return generatedCacheKey(TextureClass::Front, format, texts, paletteColour(request.seed), shas);
}

DerivedTextureCache::DerivedTextureCache(fs::path root) : m_root(std::move(root)) {}

fs::path DerivedTextureCache::entryPath(const CacheKey& key) const {
    const std::string hex = key.hex();
    return m_root / std::string(toString(key.textureClass)) / hex.substr(0, 2) / (hex + std::string(kEntryExtension));
}

Lookup DerivedTextureCache::lookup(const CacheKey& key, const std::optional<SourceStamp>& source,
                                   bool sourceUnavailable) {
    const fs::path path = entryPath(key);
    auto bytes = core::readBinaryFile(path, kMaxRvtexFileBytes);
    if (!bytes) {
        if (bytes.error().code != ErrorCode::NotFound) {
            spdlog::debug("Derived-texture cache: {} not read ({})", core::pathToUtf8Lossy(path),
                          bytes.error().toString());
        }
        ++m_misses;
        return {LookupOutcome::Missing, std::nullopt};
    }
    auto texture = parseRvtex(*bytes, &key.digest, core::pathToUtf8Lossy(path));
    if (!texture || texture->textureClass != key.textureClass) {
        spdlog::info("Derived-texture cache: deleting an entry that fails its checks ({})",
                     texture ? std::string("another class") : texture.error().message);
        std::error_code ignored;
        fs::remove(path, ignored);
        ++m_corrupt;
        return {LookupOutcome::Corrupt, std::nullopt};
    }
    if (source && texture->source != source) {
        ++m_stale;
        return {LookupOutcome::Stale, std::nullopt};
    }
    ++m_hits;
    if (sourceUnavailable) {
        ++m_hitsWithoutSource;
    }
    markServed(key);
    return {LookupOutcome::Hit, std::move(*texture)};
}

Status DerivedTextureCache::store(const CacheKey& key, const RvTexture& texture) {
    auto bytes = serializeRvtex(texture, key.digest);
    if (!bytes) {
        ++m_storeFailures;
        return propagate(bytes.error());
    }
    const fs::path path = entryPath(key);
    if (auto written = core::writeFileAtomic(path, *bytes); !written) {
        ++m_storeFailures;
        return written;
    }
    ++m_stored;
    std::lock_guard lock(m_servedMutex);
    m_served.insert(key.hex()); // written now: its time is already this run's
    return ok();
}

void DerivedTextureCache::markServed(const CacheKey& key) {
    std::string hex = key.hex();
    {
        std::lock_guard lock(m_servedMutex);
        if (!m_served.insert(std::move(hex)).second) {
            return;
        }
    }
    std::error_code error;
    fs::last_write_time(entryPath(key), fs::file_time_type::clock::now(), error);
    if (error) {
        spdlog::debug("Derived-texture cache: the time of an entry could not be set ({})", error.message());
    }
}

TrimReport DerivedTextureCache::trim(std::uint64_t maxBytes, const std::unordered_set<std::string>& inUse) {
    TrimReport report;
    struct Entry {
        fs::path path;
        std::uint64_t size = 0;
        fs::file_time_type time;
        std::string hex;
    };
    std::vector<Entry> entries;
    const auto now = fs::file_time_type::clock::now();
    std::error_code error;
    fs::recursive_directory_iterator it(m_root, fs::directory_options::skip_permission_denied, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError) || entryError) {
            continue;
        }
        const fs::path& path = it->path();
        const std::string name = core::pathToUtf8Lossy(path.filename());
        const auto time = it->last_write_time(entryError);
        if (entryError) {
            continue;
        }
        if (name.find(std::string(kEntryExtension) + ".tmp-") != std::string::npos) {
            if (now - time > kTemporaryFileAge) {
                std::error_code removeError;
                if (fs::remove(path, removeError)) {
                    ++report.temporaryFilesRemoved;
                } else {
                    ++report.failures;
                }
            }
            continue;
        }
        if (path.extension() != kEntryExtension || !isHexName(core::pathToUtf8Lossy(path.stem()))) {
            continue;
        }
        const std::uint64_t size = it->file_size(entryError);
        if (entryError) {
            continue;
        }
        entries.push_back({path, size, time, core::pathToUtf8Lossy(path.stem())});
        report.bytesBefore += size;
    }
    report.entries = entries.size();
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.time != b.time ? a.time < b.time : a.path < b.path;
    });
    std::uint64_t total = report.bytesBefore;
    for (const Entry& entry : entries) {
        if (total <= maxBytes) {
            break;
        }
        if (inUse.contains(entry.hex)) {
            ++report.keptInUse;
            continue;
        }
        std::error_code removeError;
        if (fs::remove(entry.path, removeError)) {
            ++report.removed;
            total -= entry.size;
        } else {
            ++report.failures;
        }
    }
    report.bytesAfter = total;
    return report;
}

CacheStats DerivedTextureCache::stats() const noexcept {
    return {m_hits.load(), m_hitsWithoutSource.load(), m_misses.load(), m_stale.load(),
            m_corrupt.load(), m_stored.load(), m_storeFailures.load()};
}

std::string_view toString(ServeOutcome outcome) noexcept {
    switch (outcome) {
    case ServeOutcome::CacheHit:
        return "cache-hit";
    case ServeOutcome::CacheHitSourceUnavailable:
        return "cache-hit-source-unavailable";
    case ServeOutcome::Built:
        return "built";
    case ServeOutcome::Rebuilt:
        return "rebuilt";
    }
    return "unknown";
}

Expected<ServedLayer> serveCover(DerivedTextureCache& cache, TextureFormat format, std::string_view mediaKind,
                                 std::string_view mediaRelativePath, const fs::path& sourcePath,
                                 const FitOptions& options) {
    const CacheKey key = coverCacheKey(TextureClass::Front, format, mediaKind, mediaRelativePath);
    const auto serveWithoutSource = [&](const Error& why) -> Expected<ServedLayer> {
        Lookup found = cache.lookup(key, std::nullopt, true);
        if (found.outcome == LookupOutcome::Hit) {
            return ServedLayer{std::move(*found.texture), ServeOutcome::CacheHitSourceUnavailable};
        }
        return propagate(why);
    };

    auto reader = core::SharedFileReader::open(sourcePath);
    if (!reader) {
        return serveWithoutSource(reader.error());
    }
    auto facts = reader->facts();
    if (!facts) {
        return serveWithoutSource(facts.error());
    }
    const SourceStamp stamp{facts->size, facts->lastWriteTime};
    Lookup found = cache.lookup(key, stamp);
    if (found.outcome == LookupOutcome::Hit) {
        return ServedLayer{std::move(*found.texture), ServeOutcome::CacheHit};
    }
    const std::string context = core::pathToUtf8Lossy(sourcePath);
    if (facts->size > kMaxImageFileBytes) {
        return makeError(ErrorCode::OutOfRange, "the cover file is larger than " + std::to_string(kMaxImageFileBytes) +
                                                    " bytes",
                         context);
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(facts->size));
    if (auto read = reader->read(0, bytes); !read) {
        return serveWithoutSource(read.error());
    }
    auto layer = buildCoverLayer(bytes, format, context, options);
    if (!layer) {
        return propagate(layer.error());
    }
    layer->texture.source = stamp;
    if (auto stored = cache.store(key, layer->texture); !stored) {
        spdlog::warn("Derived-texture cache: an entry could not be written ({})", stored.error().message);
    }
    return ServedLayer{std::move(layer->texture),
                       found.outcome == LookupOutcome::Missing ? ServeOutcome::Built : ServeOutcome::Rebuilt};
}

Expected<ServedLayer> serveSpine(DerivedTextureCache& cache, const LabelFonts& fonts, const SpineRequest& request,
                                 TextureFormat format) {
    return serveGenerated(cache, spineCacheKey(request, format, fonts),
                          [&] { return buildSpineLayer(fonts, request, format); });
}

Expected<ServedLayer> serveFrontLabel(DerivedTextureCache& cache, const LabelFonts& fonts,
                                      const FrontLabelRequest& request, TextureFormat format) {
    return serveGenerated(cache, frontLabelCacheKey(request, format, fonts),
                          [&] { return buildFrontLabelLayer(fonts, request, format); });
}

} // namespace rv::assets
