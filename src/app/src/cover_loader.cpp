#include "cover_loader.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/render/collection_arrays.hpp>

#include "collection_instance.hpp"

namespace rv::app {
namespace {

/// Layers made and not uploaded yet, at most (the jobs' and the arrays' queue): about 10 MiB of BC1 pictures.
constexpr std::size_t kMaxQueuedLayers = 256;

} // namespace

/// What the jobs and the loader share: the cache, the fonts (loaded by the first job that needs them), the finished
/// items.
struct CoverLoader::Shared {
    explicit Shared(std::filesystem::path cacheRoot) : cache(std::move(cacheRoot)) {}

    assets::DerivedTextureCache cache;
    std::once_flag fontsLoaded;
    std::optional<assets::FontFace> title;
    std::optional<assets::FontFace> detail;
    std::optional<Error> fontError;

    std::mutex mutex;
    std::deque<std::pair<std::size_t, ItemLayers>> finished;
    std::function<void()> wake;

    void loadFonts(const std::filesystem::path& assetRoot) {
        std::call_once(fontsLoaded, [&] {
            auto bold = assets::FontFace::loadBundled(assetRoot, assets::kLabelTitleFontPath);
            auto regular = assets::FontFace::loadBundled(assetRoot, assets::kLabelDetailFontPath);
            if (bold && regular) {
                title = std::move(*bold);
                detail = std::move(*regular);
            } else {
                fontError = bold ? regular.error() : bold.error();
            }
        });
    }

    [[nodiscard]] assets::LabelFonts fonts() const noexcept {
        return assets::LabelFonts{title ? &*title : nullptr, detail ? &*detail : nullptr};
    }
};

CoverLoader::CoverLoader(Options options, core::WorkerPool& pool, std::vector<CoverItem> items)
    : m_options(std::move(options)), m_pool(pool), m_shared(std::make_shared<Shared>(m_options.cacheRoot)),
      m_items(std::move(items)) {
    m_shared->wake = m_options.wake;
    if (m_options.jobsInFlight == 0) {
        m_options.jobsInFlight = std::max<std::size_t>(2, 2 * pool.threadCount());
    }
    m_done = m_items.empty();
    spdlog::info("Cover loader: {} item(s), {} job(s) at a time, layers {}{}", m_items.size(), m_options.jobsInFlight,
                 gl::toString(m_options.format),
                 m_options.mediaRoot ? "" : ", the media root unknown: the cache alone serves the covers");
    submitJobs(0);
}

CoverLoader::~CoverLoader() {
    m_pool.cancel(m_generation);
    const std::lock_guard lock(m_shared->mutex);
    m_shared->wake = nullptr;
}

void CoverLoader::submitJobs(std::size_t queuedUploads) {
    // The arrays' queue is not flooded either: no job starts while many layers wait for their upload.
    while (m_next < m_items.size() && m_inFlight < m_options.jobsInFlight &&
           queuedUploads + 2 * m_inFlight < kMaxQueuedLayers) {
        core::JobOptions options;
        options.generation = &m_generation;
        const std::size_t index = m_next;
        const Status submitted = m_pool.submit(
            [shared = m_shared, item = m_items[index], index, assetRoot = m_options.assetRoot,
             format = m_options.format, mediaRoot = m_options.mediaRoot](const core::JobContext& job) {
                if (job.cancelled()) {
                    return;
                }
                shared->loadFonts(assetRoot);
                ItemLayers layers = serveItemLayers(shared->cache, shared->fonts(), format, mediaRoot, item);
                std::function<void()> wake;
                {
                    const std::lock_guard lock(shared->mutex);
                    shared->finished.emplace_back(index, std::move(layers));
                    wake = shared->wake;
                }
                if (wake) {
                    wake();
                }
            },
            options);
        if (!submitted) {
            if (submitted.error().code != ErrorCode::Unavailable) {
                spdlog::error("Cover loader: a job cannot be submitted: {}", submitted.error().toString());
            }
            return; // a full queue: tried again next round
        }
        ++m_next;
        ++m_inFlight;
    }
}

void CoverLoader::startTrim() {
    if (std::exchange(m_trimStarted, true)) {
        return;
    }
    std::unordered_set<std::string> inUse(m_entries.begin(), m_entries.end());
    core::JobOptions options;
    options.generation = &m_generation;
    const Status submitted = m_pool.submit(
        [shared = m_shared, inUse = std::move(inUse)](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            const assets::TrimReport report = shared->cache.trim(assets::kDefaultTextureCacheBytes, inUse);
            spdlog::info("Texture cache trimmed: {} entries, {:.1f} MiB before, {:.1f} MiB after, {} removed, {} kept "
                         "in use, {} failure(s)",
                         report.entries, static_cast<double>(report.bytesBefore) / (1024.0 * 1024.0),
                         static_cast<double>(report.bytesAfter) / (1024.0 * 1024.0), report.removed, report.keptInUse,
                         report.failures);
        },
        options);
    if (!submitted) {
        spdlog::warn("Cover loader: the cache's trim cannot be submitted: {}", submitted.error().toString());
    }
}

Status CoverLoader::update(CollectionInstance& collection, const gl::UploadBudget& budget, std::uint64_t frame) {
    if (m_done) {
        return ok();
    }
    std::deque<std::pair<std::size_t, ItemLayers>> finished;
    {
        const std::lock_guard lock(m_shared->mutex);
        finished.swap(m_shared->finished);
    }
    gl::CollectionArrays& arrays = collection.arrays();
    for (auto& [index, layers] : finished) {
        --m_inFlight;
        ++m_processed;
        const CoverItem& item = m_items[index];
        const std::string game = item.game.toString();
        switch (layers.frontSource) {
        case FrontSource::Cover:
            ++m_covers;
            break;
        case FrontSource::CachedCover:
            ++m_cachedCovers;
            break;
        case FrontSource::Label:
            ++m_labels;
            break;
        case FrontSource::LabelForCover:
            ++m_labels;
            ++m_coverFailures;
            break;
        }
        if (layers.coverError) {
            // The error names the cover's file, one of the user's: at debug level only.
            spdlog::debug("Cover of {} not used: {}", game, layers.coverError->toString());
        }
        if (layers.error) {
            spdlog::warn("Layers of {}: {}", game, layers.error->message);
        }
        const auto submit = [&](gl::CollectionClass cls, std::optional<gl::LayerImage>& image, const std::string& key) {
            if (!image) {
                ++m_failedLayers;
                return;
            }
            if (Status submitted = arrays.submit(cls, key, std::move(*image)); !submitted) {
                ++m_failedLayers;
                spdlog::warn("Layer {} not queued: {}", key, submitted.error().toString());
            }
        };
        submit(gl::CollectionClass::Front, layers.front, item.frontKey);
        submit(gl::CollectionClass::Spine, layers.spine, item.spineKey);
        for (std::string* entry : {&layers.frontEntry, &layers.spineEntry}) {
            if (!entry->empty()) {
                m_entries.push_back(std::move(*entry));
            }
        }
    }
    const gl::LayerUploadResult uploads = arrays.uploadPending(budget);
    if (!uploads.completed.empty()) {
        ++m_uploadRounds;
        m_maxLayersPerRound = std::max<std::uint64_t>(m_maxLayersPerRound, uploads.completed.size());
    }
    for (const gl::LayerUpload& upload : uploads.completed) {
        if (Status written = collection.layerResident(upload); !written) {
            return written;
        }
    }
    submitJobs(arrays.pendingUploads());
    if (m_processed == m_items.size() && arrays.pendingUploads() == 0) {
        m_done = true;
        m_allResidentAt = std::chrono::system_clock::now();
        m_allResidentFrame = frame;
        spdlog::info("Frame {}: the room's {} item(s) have their layers: {} cover(s), {} from the cache without their "
                     "cover, {} generated label(s) ({} for a cover that could not be used), {} layer(s) not made",
                     frame, m_items.size(), m_covers, m_cachedCovers, m_labels, m_coverFailures, m_failedLayers);
        if (m_shared->fontError) {
            spdlog::error("The label fonts are refused, so no spine or label is generated: {}",
                          m_shared->fontError->toString());
        }
        startTrim();
    }
    return ok();
}

void CoverLoader::record(ResidencyRecord& record, const CollectionInstance& collection) const {
    const gl::CollectionArrays& arrays = collection.arrays();
    record.bound = true;
    record.format = std::string(gl::toString(arrays.format()));
    record.arrayBytes = arrays.estimatedBytes();
    record.items = m_items.size();
    record.frontsResident = arrays.residentLayers(gl::CollectionClass::Front);
    record.spinesResident = arrays.residentLayers(gl::CollectionClass::Spine);
    record.pending =
        2 * m_items.size() - std::min<std::size_t>(2 * m_items.size(), record.frontsResident + record.spinesResident);
    record.covers = m_covers;
    record.cachedCovers = m_cachedCovers;
    record.labels = m_labels;
    record.coverFailures = m_coverFailures;
    record.failed = m_failedLayers;
    const assets::CacheStats stats = m_shared->cache.stats();
    record.cacheHits = stats.hits;
    record.cacheHitsWithoutSource = stats.hitsWithoutSource;
    record.cacheMisses = stats.misses;
    record.cacheStale = stats.stale;
    record.cacheCorrupt = stats.corrupt;
    record.cacheStored = stats.stored;
    record.cacheStoreFailures = stats.storeFailures;
    record.allResidentFrame = m_allResidentFrame;
    record.uploadRounds = m_uploadRounds;
    record.maxLayersPerRound = m_maxLayersPerRound;
}

} // namespace rv::app
