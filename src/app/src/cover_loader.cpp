#include "cover_loader.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/assets/derived_texture_cache.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/render/collection_arrays.hpp>

#include "collection_instance.hpp"

namespace rv::app {
namespace {

/// Layers made and not uploaded yet, at most (the made items' and the arrays' queue): about 10 MiB of BC1 pictures.
constexpr std::size_t kMaxQueuedLayers = 256;
/// Jobs on the pool at a time, at least: the results are taken once a frame, so a warm cache (a job of about 0.5 ms)
/// needs enough of them to keep the upload budget's 32 items a frame fed.
constexpr std::size_t kMinJobsInFlight = 48;
/// Why an item is served again (CoverLoader::m_retrying): a retry after a refresh, a previous loader's item adopted.
constexpr std::uint8_t kRetried = 1;
constexpr std::uint8_t kAdopted = 2;

/// A layer served from the cache as it was: the same picture as the entry of the same name shown before.
[[nodiscard]] bool servedAsBefore(const std::optional<assets::ServeOutcome>& outcome) noexcept {
    return outcome == assets::ServeOutcome::CacheHit || outcome == assets::ServeOutcome::CacheHitSourceUnavailable;
}

[[nodiscard]] double mebibytes(std::uint64_t bytes) noexcept {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

} // namespace

/// What the jobs and the loader share: the cache, the fonts (loaded by the first job that needs them), the finished
/// items, the media root's check; a condition the lockstep waits on.
struct CoverLoader::Shared {
    explicit Shared(std::filesystem::path cacheRoot) : cache(std::move(cacheRoot)) {}

    assets::DerivedTextureCache cache;
    std::once_flag fontsLoaded;
    std::optional<assets::FontFace> title;
    std::optional<assets::FontFace> detail;
    std::optional<Error> fontError;

    mutable std::mutex mutex;
    mutable std::condition_variable posted;
    std::deque<std::pair<std::size_t, ItemLayers>> finished;
    /// The media root checked by retry(): the root, and whether it can be read.
    std::optional<std::pair<std::filesystem::path, bool>> rootChecked;
    std::uint64_t posts = 0;
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

    /// Posts under the lock, then wakes the host and a lockstep wait.
    template <typename Fill>
    void post(Fill&& fill) {
        std::function<void()> wakeHost;
        {
            const std::lock_guard lock(mutex);
            fill(*this);
            ++posts;
            wakeHost = wake;
        }
        posted.notify_all();
        if (wakeHost) {
            wakeHost();
        }
    }
};

CoverLoader::CoverLoader(Options options, core::WorkerPool& pool, std::vector<CoverItem> items,
                         std::vector<core::Aabb> bounds, std::vector<std::uint32_t> groups)
    : m_options(std::move(options)), m_pool(pool), m_shared(std::make_shared<Shared>(m_options.cacheRoot)),
      m_items(std::move(items)), m_groups([&] {
          groups.resize(m_items.size(), 0u);
          return std::move(groups);
      }()),
      m_schedule(
          [&] {
              bounds.resize(m_items.size());
              return std::move(bounds);
          }(),
          m_groups),
      m_made(m_items.size()), m_front(m_items.size()), m_frontEntry(m_items.size()), m_spineEntry(m_items.size()),
      m_retrying(m_items.size(), 0) {
    m_shared->wake = m_options.wake;
    for (std::size_t index = 0; index < m_items.size(); ++index) {
        m_byKey.emplace(m_items[index].frontKey, index);
        m_byKey.emplace(m_items[index].spineKey, index);
    }
    if (m_options.jobsInFlight == 0) {
        m_options.jobsInFlight = std::max<std::size_t>(kMinJobsInFlight, 2 * pool.threadCount());
    }
    spdlog::info("Cover loader: {} item(s), {} job(s) at a time, layers {}{}", m_items.size(), m_options.jobsInFlight,
                 gl::toString(m_options.format),
                 m_options.mediaRoot ? "" : ", the media root unknown: the cache alone serves the covers");
}

CoverLoader::~CoverLoader() {
    m_pool.cancel(m_generation);
    const std::lock_guard lock(m_shared->mutex);
    m_shared->wake = nullptr;
}

bool CoverLoader::done() const noexcept {
    return m_schedule.done() && !m_rootCheckPending;
}

bool CoverLoader::groupDone(std::uint32_t group) const {
    for (std::size_t item = 0; item < m_schedule.size(); ++item) {
        if (groupOf(item) == group && m_schedule.stage(item) != CoverSchedule::Stage::Done) {
            return false;
        }
    }
    return true;
}

void CoverLoader::waitForResults(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(m_shared->mutex);
    const std::uint64_t seen = m_shared->posts;
    m_shared->posted.wait_for(lock, timeout, [&] {
        return m_shared->posts != seen || !m_shared->finished.empty() || m_shared->rootChecked.has_value();
    });
}

void CoverLoader::submitJobs() {
    const std::size_t running = m_schedule.makingCount();
    if (running >= m_options.jobsInFlight) {
        return;
    }
    // The made layers are not let pile up: no job starts while many wait for their upload.
    if (m_schedule.madeLayers() + 2 * running >= kMaxQueuedLayers) {
        return;
    }
    const std::vector<std::size_t> next = m_schedule.takeToMake(m_options.jobsInFlight - running);
    for (const std::size_t index : next) {
        core::JobOptions options;
        options.generation = &m_generation;
        // By rank in the pool too: an item that came into view since goes before the queued rest of the room.
        options.priority = 2 - static_cast<std::int64_t>(m_schedule.rank(index));
        const Status submitted = m_pool.submit(
            [shared = m_shared, item = m_items[index], index, assetRoot = m_options.assetRoot,
             format = m_options.format, mediaRoot = m_options.mediaRoot](const core::JobContext& job) {
                if (job.cancelled()) {
                    return;
                }
                shared->loadFonts(assetRoot);
                ItemLayers layers = serveItemLayers(shared->cache, shared->fonts(), format, mediaRoot, item);
                if (job.cancelled()) {
                    return;
                }
                shared->post([&](Shared& mailbox) { mailbox.finished.emplace_back(index, std::move(layers)); });
            },
            options);
        if (!submitted) {
            if (submitted.error().code != ErrorCode::Unavailable) {
                spdlog::error("Cover loader: a job cannot be submitted: {}", submitted.error().toString());
            }
            // A full queue: the item waits for a later round.
            m_schedule.giveBack(index);
        }
    }
}

void CoverLoader::takeFinished() {
    std::deque<std::pair<std::size_t, ItemLayers>> finished;
    {
        const std::lock_guard lock(m_shared->mutex);
        finished.swap(m_shared->finished);
    }
    for (auto& [index, layers] : finished) {
        if (index >= m_items.size() || m_schedule.stage(index) != CoverSchedule::Stage::Making) {
            continue;
        }
        const CoverItem& item = m_items[index];
        const std::string game = item.game.toString();
        if (layers.coverError) {
            // The error names the cover's file, one of the user's: at debug level only.
            spdlog::debug("Cover of {} not used: {}", game, layers.coverError->toString());
        }
        if (layers.error) {
            spdlog::warn("Layers of {}: {}", game, layers.error->message);
        }
        const std::uint8_t again = std::exchange(m_retrying[index], std::uint8_t{0});
        const bool retried = again != 0;
        if (retried) {
            // A retry uploads only what changed: a layer served from the cache under the name shown before is the
            // same picture.
            const bool frontSame =
                !layers.front || (layers.frontEntry == m_frontEntry[index] && servedAsBefore(layers.frontOutcome));
            const bool spineSame =
                !layers.spine || (layers.spineEntry == m_spineEntry[index] && servedAsBefore(layers.spineOutcome));
            if (frontSame) {
                layers.front.reset();
            }
            if (spineSame) {
                layers.spine.reset();
            }
            if (!frontSame || !spineSame) {
                ++(again == kAdopted ? m_adoptedChanged : m_retriedChanged);
            }
        } else {
            m_failedLayers += (layers.front ? 0 : 1) + (layers.spine ? 0 : 1);
        }
        // What the front shows now (a retry that could not serve it at all leaves it as it was).
        if (!layers.frontEntry.empty() || !retried) {
            m_front[index] = layers.frontSource;
        }
        for (auto [entry, kept] : {std::pair{&layers.frontEntry, &m_frontEntry[index]},
                                   std::pair{&layers.spineEntry, &m_spineEntry[index]}}) {
            if (!entry->empty()) {
                *kept = std::move(*entry);
            }
        }
        const unsigned count = (layers.front ? 1u : 0u) + (layers.spine ? 1u : 0u);
        if (m_schedule.made(index, count) && count > 0) {
            m_made[index] = std::move(layers);
        }
    }
}

std::optional<std::size_t> CoverLoader::itemOfKey(const std::string& key) const {
    const auto found = m_byKey.find(key);
    return found != m_byKey.end() ? std::optional<std::size_t>(found->second) : std::nullopt;
}

void CoverLoader::submitUploads(std::span<CollectionInstance* const> collections, std::size_t maxLayers) {
    for (const std::size_t index : m_schedule.takeToUpload(maxLayers)) {
        std::optional<ItemLayers> layers = std::exchange(m_made[index], std::nullopt);
        const CoverItem& item = m_items[index];
        const std::uint32_t group = groupOf(index);
        if (group >= collections.size() || collections[group] == nullptr) {
            // No collection to take its layers: the item is done without them.
            m_schedule.layerDone(index);
            m_schedule.layerDone(index);
            continue;
        }
        gl::CollectionArrays& arrays = collections[group]->arrays();
        const auto submit = [&](gl::CollectionClass cls, std::optional<gl::LayerImage>& image, const std::string& key) {
            if (!image) {
                return;
            }
            if (Status submitted = arrays.submit(cls, key, std::move(*image)); !submitted) {
                ++m_failedLayers;
                spdlog::warn("Layer {} not queued: {}", key, submitted.error().toString());
                m_schedule.layerDone(index);
            }
        };
        if (!layers) {
            continue;
        }
        submit(gl::CollectionClass::Front, layers->front, item.frontKey);
        submit(gl::CollectionClass::Spine, layers->spine, item.spineKey);
    }
}

void CoverLoader::adoptShown(const CoverLoader& previous, std::span<CollectionInstance* const> collections) {
    std::size_t adopted = 0;
    std::vector<std::size_t> resident;
    const bool sameRoot = previous.m_options.mediaRoot == m_options.mediaRoot;
    // By game: the live update (I3 step 15b) adopts the items of a collection that gained or lost some.
    std::map<catalog::GameId, std::size_t> before;
    for (std::size_t index = 0; index < previous.m_items.size(); ++index) {
        before.emplace(previous.m_items[index].game, index);
    }
    for (std::size_t index = 0; index < m_items.size(); ++index) {
        // An item whose layers the previous loader had made (uploaded or not: the arrays upload what it queued).
        const auto found = before.find(m_items[index].game);
        if (found == before.end()) {
            continue;
        }
        const std::size_t was = found->second;
        if (!previous.m_front[was] || previous.m_schedule.stage(was) != CoverSchedule::Stage::Done) {
            continue;
        }
        m_front[index] = previous.m_front[was];
        m_frontEntry[index] = previous.m_frontEntry[was];
        m_spineEntry[index] = previous.m_spineEntry[was];
        ++adopted;
        // I4 step 6b: asked as it was, its pictures in its collection, its front needing no new check: done as it is.
        const FrontSource front = *previous.m_front[was];
        const bool settled = sameRoot || front == FrontSource::Cover || front == FrontSource::Label;
        const std::uint32_t group = groupOf(index);
        const CollectionInstance* collection = group < collections.size() ? collections[group] : nullptr;
        if (settled && previous.m_items[was] == m_items[index] && collection != nullptr) {
            const auto frontSlot = collection->arrays().find(gl::CollectionClass::Front, m_items[index].frontKey);
            const auto spineSlot = collection->arrays().find(gl::CollectionClass::Spine, m_items[index].spineKey);
            if (frontSlot && frontSlot->resident && spineSlot && spineSlot->resident) {
                resident.push_back(index);
                continue;
            }
        }
        m_retrying[index] = kAdopted;
    }
    m_schedule.adoptDone(resident);
    m_adopted += adopted;
    m_adoptedResident += resident.size();
    spdlog::info("Cover loader: {} of {} item(s) shown before, {} of them as they show (nothing served again); the "
                 "others' pictures are uploaded again only if they changed",
                 adopted, m_items.size(), resident.size());
}

void CoverLoader::retry(std::filesystem::path mediaRoot) {
    if (m_rootCheckPending) {
        return;
    }
    core::JobOptions options;
    options.generation = &m_generation;
    options.priority = 1;
    const Status submitted = m_pool.submit(
        [shared = m_shared, root = std::move(mediaRoot)](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            std::error_code error;
            const bool readable =
                std::filesystem::is_directory(std::filesystem::path(core::extendedLengthPath(root)), error) && !error;
            shared->post([&](Shared& mailbox) { mailbox.rootChecked = std::pair{root, readable}; });
        },
        options);
    if (!submitted) {
        spdlog::warn("Cover loader: the media root's check cannot be submitted: {}", submitted.error().toString());
        return;
    }
    m_rootCheckPending = true;
}

void CoverLoader::takeRootCheck() {
    std::optional<std::pair<std::filesystem::path, bool>> checked;
    {
        const std::lock_guard lock(m_shared->mutex);
        checked = std::exchange(m_shared->rootChecked, std::nullopt);
    }
    if (!checked) {
        return;
    }
    m_rootCheckPending = false;
    if (!checked->second) {
        spdlog::info("Cover loader: the media root still cannot be read; the covers stay as they are");
        return;
    }
    m_options.mediaRoot = std::move(checked->first);
    std::size_t queued = 0;
    for (std::size_t index = 0; index < m_items.size(); ++index) {
        const bool fromCover = m_front[index] == FrontSource::Cover || m_front[index] == FrontSource::Label;
        if (fromCover || m_items[index].coverPath.empty() || !m_schedule.requeue(index)) {
            continue;
        }
        m_retrying[index] = kRetried;
        ++queued;
    }
    ++m_retryRounds;
    m_retriedItems += queued;
    spdlog::info("Cover loader: the media root can be read; {} item(s) whose front did not come from its cover served "
                 "again",
                 queued);
}

void CoverLoader::startTrim() {
    if (std::exchange(m_trimStarted, true)) {
        return;
    }
    std::unordered_set<std::string> inUse;
    for (const std::vector<std::string>* entries : {&m_frontEntry, &m_spineEntry}) {
        for (const std::string& entry : *entries) {
            if (!entry.empty()) {
                inUse.insert(entry);
            }
        }
    }
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
                         report.entries, mebibytes(report.bytesBefore), mebibytes(report.bytesAfter), report.removed,
                         report.keptInUse, report.failures);
        },
        options);
    if (!submitted) {
        spdlog::warn("Cover loader: the cache's trim cannot be submitted: {}", submitted.error().toString());
    }
}

void CoverLoader::noteArrivals() {
    if (!m_viewItems) {
        m_viewItems = m_schedule.inViewCount();
        for (std::size_t item = 0; item < m_items.size(); ++item) {
            if (m_schedule.inView(item)) {
                const std::uint32_t group = groupOf(item);
                if (group >= m_viewItemsByGroup.size()) {
                    m_viewItemsByGroup.resize(static_cast<std::size_t>(group) + 1, 0u);
                }
                ++m_viewItemsByGroup[group];
            }
        }
    }
    if (!m_viewResidentAt && m_schedule.viewDone()) {
        m_viewResidentAt = std::chrono::system_clock::now();
        m_residentAtViewResident = m_schedule.doneCount();
        spdlog::info("Cover loader: the {} item(s) in view are resident ({} of {} in all)", m_schedule.inViewCount(),
                     m_schedule.doneCount(), m_items.size());
    }
}

Status CoverLoader::update(std::span<CollectionInstance* const> collections, const Round& round) {
    m_schedule.setView(round.view);
    m_schedule.setFocused(round.focused);
    m_schedule.setPaused(round.paused);
    takeRootCheck();
    if (m_schedule.done() && m_wasDone) {
        return ok();
    }
    takeFinished();
    if (round.paused) {
        // A launched game runs: no job starts and nothing goes up (the running jobs finish, their layers wait).
        ++m_pausedRounds;
        return ok();
    }
    // The made layers go up in the schedule's order: only a budget's worth is handed to the arrays at a time.
    std::size_t queued = 0;
    for (const CollectionInstance* collection : collections) {
        queued += collection != nullptr ? collection->arrays().pendingUploads() : 0;
    }
    if (queued < round.budget.layers) {
        submitUploads(collections, round.budget.layers - queued);
    }
    gl::UploadBudget budget = round.budget;
    budget.time = round.budget.time > round.spent ? round.budget.time - round.spent : std::chrono::nanoseconds(0);
    // One budget for the floor: each room's queue in turn while it lasts (the first call always uploads one layer, as
    // a single room's did).
    gl::LayerUploadResult uploads;
    std::vector<std::pair<std::size_t, gl::LayerUpload>> completed;
    bool first = true;
    for (std::size_t group = 0; group < collections.size(); ++group) {
        CollectionInstance* collection = collections[group];
        if (collection == nullptr || collection->arrays().pendingUploads() == 0) {
            continue;
        }
        if (!first && (budget.time <= std::chrono::nanoseconds(0) || budget.layers == 0)) {
            break;
        }
        first = false;
        gl::LayerUploadResult part = collection->arrays().uploadPending(budget);
        budget.time = budget.time > part.elapsed ? budget.time - part.elapsed : std::chrono::nanoseconds(0);
        budget.layers = budget.layers > part.completed.size() ? budget.layers - part.completed.size() : 0;
        uploads.elapsed += part.elapsed;
        uploads.dropped += part.dropped;
        uploads.pending += part.pending;
        for (gl::LayerUpload& upload : part.completed) {
            completed.emplace_back(group, upload);
            uploads.completed.push_back(std::move(upload));
        }
    }
    if (!uploads.completed.empty()) {
        ++m_uploadRounds;
        // The frame's budget bounds a frame's uploads; an idle wake of a lockstep run has a larger budget of its own.
        if (round.frame) {
            m_maxLayersPerRound = std::max<std::uint64_t>(m_maxLayersPerRound, uploads.completed.size());
        }
    }
    if (round.frame && (!uploads.completed.empty() || round.spent.count() > 0)) {
        const double milliseconds = std::chrono::duration<double, std::milli>(uploads.elapsed + round.spent).count();
        m_maxRoundMs = std::max(m_maxRoundMs, milliseconds);
        if (uploads.elapsed + round.spent > round.budget.time) {
            ++m_overBudgetRounds;
        }
    }
    if (uploads.dropped > 0) {
        spdlog::warn("Cover loader: {} upload(s) dropped (their layers were given to other keys)", uploads.dropped);
    }
    for (const auto& [group, upload] : completed) {
        if (Status written = collections[group]->layerResident(upload); !written) {
            return written;
        }
        if (const std::optional<std::size_t> item = itemOfKey(upload.key)) {
            m_schedule.layerDone(*item);
        }
    }
    submitJobs();
    noteArrivals();
    if (m_schedule.done() && !m_wasDone && !m_rootCheckPending) {
        m_wasDone = true;
        if (!m_allResidentAt) {
            m_allResidentAt = std::chrono::system_clock::now();
            m_allResidentFrame = round.frameIndex;
            std::size_t covers = 0;
            std::size_t cached = 0;
            std::size_t labels = 0;
            for (const std::optional<FrontSource>& source : m_front) {
                covers += source == FrontSource::Cover ? 1 : 0;
                cached += source == FrontSource::CachedCover ? 1 : 0;
                labels += source == FrontSource::Label || source == FrontSource::LabelForCover ? 1 : 0;
            }
            spdlog::info("Frame {}: the room's {} item(s) have their layers: {} cover(s), {} from the cache without "
                         "their cover, {} generated label(s), {} layer(s) not made",
                         round.frameIndex, m_items.size(), covers, cached, labels, m_failedLayers);
            if (m_shared->fontError) {
                spdlog::error("The label fonts are refused, so no spine or label is generated: {}",
                              m_shared->fontError->toString());
            }
        } else {
            spdlog::info("Frame {}: the retried items are done ({} changed)", round.frameIndex, m_retriedChanged);
        }
        startTrim();
    } else if (!m_schedule.done()) {
        m_wasDone = false;
    }
    return ok();
}

void CoverLoader::record(ResidencyRecord& record, std::span<CollectionInstance* const> collections) const {
    record.bound = true;
    record.format = std::string(gl::toString(m_options.format));
    record.arrayBytes = 0;
    record.frontsResident = 0;
    record.spinesResident = 0;
    for (const CollectionInstance* collection : collections) {
        if (collection == nullptr) {
            continue;
        }
        const gl::CollectionArrays& arrays = collection->arrays();
        record.format = std::string(gl::toString(arrays.format()));
        record.arrayBytes += arrays.estimatedBytes();
        record.frontsResident += arrays.residentLayers(gl::CollectionClass::Front);
        record.spinesResident += arrays.residentLayers(gl::CollectionClass::Spine);
    }
    record.items = m_items.size();
    record.pending =
        2 * m_items.size() - std::min<std::size_t>(2 * m_items.size(), record.frontsResident + record.spinesResident);
    record.covers = 0;
    record.cachedCovers = 0;
    record.labels = 0;
    record.coverFailures = 0;
    for (const std::optional<FrontSource>& source : m_front) {
        if (!source) {
            continue;
        }
        switch (*source) {
        case FrontSource::Cover:
            ++record.covers;
            break;
        case FrontSource::CachedCover:
            ++record.cachedCovers;
            break;
        case FrontSource::Label:
            ++record.labels;
            break;
        case FrontSource::LabelForCover:
            ++record.labels;
            ++record.coverFailures;
            break;
        }
    }
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

    record.viewItems = m_viewItems.value_or(0);
    record.residentAtViewResident = m_residentAtViewResident;
    const std::span<const CoverArrival> arrivals = m_schedule.arrivals();
    const std::size_t first = std::min<std::size_t>(record.viewItems, arrivals.size());
    record.inViewAmongFirst = static_cast<std::uint64_t>(
        std::count_if(arrivals.begin(), arrivals.begin() + static_cast<std::ptrdiff_t>(first),
                      [](const CoverArrival& arrival) { return arrival.rank != CoverRank::Rest; }));
    // The rest of the room comes by distance: the quarters of the arrivals that were out of view.
    std::vector<float> rest;
    for (const CoverArrival& arrival : arrivals) {
        if (arrival.rank == CoverRank::Rest) {
            rest.push_back(arrival.distance);
        }
    }
    for (std::size_t quarter = 0; quarter < 4; ++quarter) {
        const std::size_t begin = rest.size() * quarter / 4;
        const std::size_t end = rest.size() * (quarter + 1) / 4;
        double sum = 0.0;
        for (std::size_t index = begin; index < end; ++index) {
            sum += rest[index];
        }
        record.arrivalDistanceQuarters[quarter] = end > begin ? sum / static_cast<double>(end - begin) : 0.0;
    }
    record.overBudgetRounds = m_overBudgetRounds;
    record.maxRoundMs = m_maxRoundMs;
    record.pausedRounds = m_pausedRounds;
    record.retryRounds = m_retryRounds;
    record.retriedItems = m_retriedItems;
    record.retriedChanged = m_retriedChanged;
    record.adoptedItems = m_adopted;
    record.adoptedChanged = m_adoptedChanged;
    record.adoptedResident = m_adoptedResident;
}

std::vector<std::string> CoverLoader::statusLines(std::span<CollectionInstance* const> collections) const {
    std::size_t fronts = 0;
    std::size_t spines = 0;
    std::size_t bytes = 0;
    std::size_t uploading = 0;
    for (const CollectionInstance* collection : collections) {
        if (collection == nullptr) {
            continue;
        }
        fronts += collection->arrays().residentLayers(gl::CollectionClass::Front);
        spines += collection->arrays().residentLayers(gl::CollectionClass::Spine);
        bytes += collection->arrays().estimatedBytes();
        uploading += collection->arrays().pendingUploads();
    }
    std::size_t covers = 0;
    std::size_t cached = 0;
    std::size_t labels = 0;
    for (const std::optional<FrontSource>& source : m_front) {
        covers += source == FrontSource::Cover ? 1 : 0;
        cached += source == FrontSource::CachedCover ? 1 : 0;
        labels += source == FrontSource::Label || source == FrontSource::LabelForCover ? 1 : 0;
    }
    const assets::CacheStats stats = m_shared->cache.stats();
    std::vector<std::string> lines;
    lines.push_back(fmt::format("{} items in {} room(s): fronts {}, spines {} resident ({}, {:.1f} MiB)",
                                m_items.size(), collections.size(), fronts, spines, gl::toString(m_options.format),
                                mebibytes(bytes)));
    lines.push_back(
        fmt::format("Fronts: {} covers, {} cached without their cover, {} generated labels", covers, cached, labels));
    lines.push_back(fmt::format("Cache: {} hits ({} without their source), {} misses, {} stored", stats.hits,
                                stats.hitsWithoutSource, stats.misses, stats.stored));
    lines.push_back(fmt::format("Queue: {} making, {} made, {} uploading; in view {} ({} pending){}",
                                m_schedule.makingCount(), m_schedule.madeCount(), uploading,
                                m_schedule.inViewCount(), m_schedule.inViewPending(),
                                m_schedule.paused() ? "; paused (a game runs)" : ""));
    lines.push_back(fmt::format("Media root {}", m_options.mediaRoot ? "known" : "unknown: the cache alone serves"));
    return lines;
}

} // namespace rv::app
