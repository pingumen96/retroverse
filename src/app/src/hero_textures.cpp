#include "hero_textures.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <span>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>

namespace rv::app {

/// What the jobs and the set share: the pictures made (or why one could not be), in the order they were finished.
struct HeroTextures::Shared {
    struct Finished {
        std::string game;
        std::optional<assets::ImageData> picture;
        std::optional<Error> error;
    };

    std::mutex mutex;
    std::deque<Finished> finished;
    std::function<void()> wake;
};

HeroTextures::HeroTextures(Options options, core::WorkerPool& pool)
    : m_options(std::move(options)), m_pool(pool), m_shared(std::make_shared<Shared>()), m_lru(m_options.capacity) {
    m_shared->wake = m_options.wake;
}

HeroTextures::~HeroTextures() {
    m_pool.cancel(m_generation);
    const std::lock_guard lock(m_shared->mutex);
    m_shared->wake = nullptr;
}

void HeroTextures::setMediaRoot(std::optional<std::filesystem::path> mediaRoot) {
    m_options.mediaRoot = std::move(mediaRoot);
    m_failed.clear();
}

void HeroTextures::request(const CoverItem& item) {
    const std::string game = item.game.toString();
    if (m_textures.contains(game)) {
        m_lru.touch(game);
        return;
    }
    if (m_loading.contains(game) || m_failed.contains(game) || item.coverPath.empty() || !m_options.mediaRoot) {
        return;
    }
    auto source = core::extendedLengthPathBelow(*m_options.mediaRoot, item.coverPath);
    if (!source) {
        m_failed.emplace(game, source.error().toString());
        return;
    }
    core::JobOptions options;
    options.priority = kJobPriority;
    options.generation = &m_generation;
    const Status submitted = m_pool.submit(
        [shared = m_shared, game, path = std::move(*source), maxSide = m_options.maxSide,
         label = item.label](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            Shared::Finished finished;
            finished.game = game;
            if (auto bytes = core::readFileShared(path, assets::kMaxImageFileBytes); !bytes) {
                finished.error = bytes.error();
            } else if (auto picture =
                           makeHeroPicture(std::as_bytes(std::span(bytes->data(), bytes->size())), game, maxSide,
                                           label);
                       !picture) {
                finished.error = picture.error();
            } else {
                finished.picture = std::move(*picture);
            }
            if (job.cancelled()) {
                return;
            }
            std::function<void()> wake;
            {
                const std::lock_guard lock(shared->mutex);
                shared->finished.push_back(std::move(finished));
                wake = shared->wake;
            }
            if (wake) {
                wake();
            }
        },
        options);
    if (!submitted) {
        if (submitted.error().code != ErrorCode::Unavailable) {
            spdlog::warn("Hero textures: the job of {} cannot be submitted: {}", game, submitted.error().toString());
        }
        return; // asked again next round
    }
    ++m_requests;
    m_loading.insert(game);
}

double HeroTextures::update(bool paused, std::span<const std::string> pinned) {
    if (paused) {
        return 0.0;
    }
    std::optional<Shared::Finished> next;
    {
        const std::lock_guard lock(m_shared->mutex);
        if (!m_shared->finished.empty()) {
            next = std::move(m_shared->finished.front());
            m_shared->finished.pop_front();
        }
    }
    if (!next) {
        return 0.0;
    }
    m_loading.erase(next->game);
    if (m_discarded.erase(next->game) > 0) {
        return 0.0; // forgotten while it loaded (its cover changed: I3 step 15b); asked again when needed
    }
    if (!next->picture) {
        // The error names the cover's file, one of the user's: at debug level only.
        spdlog::debug("Hero texture of {} not made: {}", next->game, next->error ? next->error->toString() : "?");
        m_failed.emplace(next->game, next->error ? next->error->message : std::string("unknown"));
        return 0.0;
    }
    const auto start = std::chrono::steady_clock::now();
    gl::TextureDesc desc;
    desc.colorSpace = gl::ColorSpace::Srgb;
    desc.mipmaps = true;
    desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
    desc.sampling.filter = gl::TextureFilter::Trilinear;
    desc.sampling.anisotropy = m_options.anisotropy;
    desc.memoryKind = gl::GpuMemoryKind::HeroTextures;
    auto texture = gl::Texture2D::create(*next->picture, desc, fmt::format("Hero {}", next->game));
    const double milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!texture) {
        spdlog::warn("Hero texture of {} cannot be created: {}", next->game, texture.error().toString());
        m_failed.emplace(next->game, texture.error().message);
        return milliseconds;
    }
    m_maxUploadMs = std::max(m_maxUploadMs, milliseconds);
    if (const std::optional<std::string> evicted = m_lru.insert(next->game, pinned)) {
        if (*evicted == next->game) {
            // Every texture held is pinned: this one is not kept.
            return milliseconds;
        }
        m_textures.erase(*evicted);
        ++m_evicted;
    }
    m_textures.insert_or_assign(next->game, std::move(*texture));
    ++m_loaded;
    m_peakBytes = std::max<std::uint64_t>(m_peakBytes, bytes());
    spdlog::debug("Hero texture of {}: {}x{} in {:.2f} ms ({} held)", next->game, next->picture->width,
                  next->picture->height, milliseconds, m_textures.size());
    return milliseconds;
}

void HeroTextures::forget(const catalog::GameId& game) {
    const std::string key = game.toString();
    m_textures.erase(key);
    m_lru.erase(key);
    m_failed.erase(key);
    if (m_loading.contains(key)) {
        m_discarded.insert(key);
    }
}

const gl::Texture2D* HeroTextures::find(const catalog::GameId& game) const {
    const auto found = m_textures.find(game.toString());
    return found == m_textures.end() ? nullptr : &found->second;
}

bool HeroTextures::loading(const catalog::GameId& game) const {
    return m_loading.contains(game.toString());
}

std::size_t HeroTextures::bytes() const noexcept {
    std::size_t total = 0;
    for (const auto& [game, texture] : m_textures) {
        total += texture.estimatedBytes();
    }
    return total;
}

void HeroTextures::record(HeroRecord& record) const {
    record.requests = m_requests;
    record.loaded = m_loaded;
    record.failed = m_failed.size();
    record.evicted = m_evicted;
    record.resident = m_textures.size();
    record.bytes = bytes();
    record.peakBytes = m_peakBytes;
    record.maxUploadMs = m_maxUploadMs;
}

} // namespace rv::app
