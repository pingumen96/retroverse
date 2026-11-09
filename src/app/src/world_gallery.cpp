#include "world_gallery.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/scene_instance.hpp>
#include <rv/assets/bc1.hpp>
#include <rv/assets/console_content.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

#include "world_signs.hpp"

namespace rv::app {

namespace {

/// Behind every other job: the covers' priority falls with their rank from the eye (CoverLoader: 2 - rank, a few
/// thousand below 0 on a large floor), and the frames' pictures must never hold back a cover in view.
constexpr std::int64_t kJobPriority = -1'000'000;
/// Pictures uploaded a frame (about a millisecond each, compressed).
constexpr std::size_t kUploadsPerFrame = 2;
/// Pictures on the pool at once outside a lockstep frame: a picture takes a worker for tens to hundreds of
/// milliseconds (a large fanart decoded, fitted, its mips compressed), and the covers must find the others free.
constexpr std::size_t kMaxInFlight = 2;

/// What a picture of a kind takes on the GPU with its mips: BC1 (half a byte a texel) or RGBA8.
std::uint64_t estimatedBytes(world::GalleryKind kind, bool compressed) {
    const glm::ivec2 size = galleryUploadSize(kind);
    const auto texels = static_cast<std::uint64_t>(size.x) * static_cast<std::uint64_t>(size.y);
    return (compressed ? texels / 2 : texels * 4) * 4 / 3;
}

/// A picture's request: its medium and its kind (equal pictures share a texture).
std::string requestKey(const GalleryChoice& choice) {
    return fmt::format("{}|{}", world::toString(choice.kind), core::pathToUtf8Lossy(choice.media));
}

} // namespace

struct WorldGallery::Shared {
    struct Finished {
        std::string request;
        std::optional<assets::CompressedTexture> blocks;
        std::optional<assets::ImageData> picture;
        std::string error;
    };
    std::mutex mutex;
    std::vector<Finished> finished;
};

WorldGallery::WorldGallery(core::WorkerPool& pool) : m_pool(pool), m_shared(std::make_shared<Shared>()) {}

WorldGallery::~WorldGallery() {
    m_pool.cancel(m_generation);
}

std::size_t WorldGallery::shown() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(m_targets.begin(), m_targets.end(), [](const Target& target) { return target.applied; }));
}

std::uint64_t WorldGallery::bytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& [request, drawn] : m_drawn) {
        total += drawn.texture ? drawn.texture->estimatedBytes() : 0;
    }
    return total;
}

void WorldGallery::takeOff(gl::MaterialRegistry& materials) {
    for (Target& target : m_targets) {
        if (!target.applied) {
            continue;
        }
        target.applied = false;
        // Only a material that is still the frame's (a registry cleared since holds others under its handles).
        if (target.handle.value < materials.size() && materials.material(target.handle).key == target.materialKey) {
            if (Status off = materials.setTextureOverride(target.handle, gl::MaterialTextureSlot::BaseColor, 0); !off) {
                spdlog::warn("World: the frame {} keeps its picture: {}", target.objectId, off.error().toString());
            }
        }
    }
}

void WorldGallery::rescan(const Frame& frame) {
    takeOff(*frame.materials);
    m_targets.clear();
    // The jobs still queued for the scene before are dropped (a ride's floor left: its pictures would hold the pool
    // while the next floor's covers wait); those still wanted are asked again below.
    m_pool.cancel(m_generation);
    for (auto drawn = m_drawn.begin(); drawn != m_drawn.end();) {
        drawn = drawn->second.pending ? m_drawn.erase(drawn) : std::next(drawn);
    }
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->finished.clear();
    }
    const std::vector<GalleryTarget> targets = galleryTargets(*frame.description);
    m_frames = targets.size();
    m_capped = 0;
    std::uint64_t asked = 0;
    // The frames by room, in the scene's order, and each room's choice.
    std::map<std::string, std::vector<GalleryTarget>, std::less<>> rooms;
    for (const GalleryTarget& target : targets) {
        rooms[target.room].push_back(target);
    }
    std::map<std::string, std::pair<GalleryChoice, bool>, std::less<>> asks; // request -> (choice, compressed)
    if (m_enabled && frame.gallery) {
        for (const auto& [room, frames] : rooms) {
            const RoomGallery gallery = frame.gallery(room);
            for (const GalleryChoice& choice : chooseGalleryPictures(frames, gallery)) {
                std::string request = requestKey(choice);
                if (m_failed.contains(request)) {
                    continue; // its print stays
                }
                const std::optional<scene::MaterialHandle> handle =
                    objectPictureMaterial(choice.objectId, *frame.instance, *frame.materials);
                if (!handle) {
                    continue;
                }
                if (!asks.contains(request)) {
                    const std::uint64_t bytes = estimatedBytes(choice.kind, frame.compressed);
                    if (asked + bytes > kCapBytes) {
                        ++m_capped;
                        continue; // its print stays
                    }
                    asked += bytes;
                }
                m_targets.push_back(
                    Target{choice.objectId, *handle, frame.materials->material(*handle).key, request, false});
                asks.try_emplace(std::move(request), choice, frame.compressed);
            }
        }
    }
    if (m_capped > 0) {
        spdlog::warn("World: {} gallery frame(s) keep their prints: the scene's pictures reached {} MiB", m_capped,
                     kCapBytes / (1024 * 1024));
    }
    // Pictures no frame takes any more are released (their overrides are off); new requests go to the pool.
    for (auto drawn = m_drawn.begin(); drawn != m_drawn.end();) {
        drawn = asks.contains(drawn->first) ? std::next(drawn) : m_drawn.erase(drawn); // a late result is dropped
    }
    m_queue.clear();
    for (const auto& [request, ask] : asks) {
        if (m_drawn.contains(request)) {
            continue;
        }
        m_drawn.emplace(request, Drawn{std::nullopt, true, false});
        m_queue.push_back(Ask{request, ask.first, ask.second});
    }
}

void WorldGallery::pump(bool settle) {
    std::size_t inFlight = 0;
    for (const auto& [request, drawn] : m_drawn) {
        inFlight += drawn.pending && drawn.submitted ? 1 : 0;
    }
    while (!m_queue.empty() && (settle || inFlight < kMaxInFlight)) {
        Ask ask = std::move(m_queue.front());
        m_queue.erase(m_queue.begin());
        const auto drawn = m_drawn.find(ask.request);
        if (drawn == m_drawn.end() || !drawn->second.pending) {
            continue; // no frame takes it any more
        }
        core::JobOptions options;
        options.priority = kJobPriority;
        options.generation = &m_generation;
        const Status submitted = m_pool.submit(
            [shared = m_shared, request = ask.request, choice = ask.choice,
             compressed = ask.compressed](const core::JobContext& job) {
                if (job.cancelled()) {
                    return;
                }
                Shared::Finished finished{request, std::nullopt, std::nullopt, {}};
                if (auto bytes = core::readFileShared(choice.media, assets::kMaxImageFileBytes); !bytes) {
                    finished.error = bytes.error().toString();
                } else if (auto picture = makeGalleryPicture(std::as_bytes(std::span(bytes->data(), bytes->size())),
                                                             choice.kind, core::pathToUtf8Lossy(choice.media));
                           !picture) {
                    finished.error = picture.error().toString();
                } else if (compressed) {
                    assets::CompressedTexture blocks;
                    blocks.srgb = true;
                    blocks.format = assets::TextureBlocks::Bc1Srgb;
                    for (const assets::ImageData& level : assets::buildMipChain(*picture)) {
                        if (job.cancelled()) {
                            return;
                        }
                        blocks.levels.push_back(
                            assets::CompressedLevel{level.width, level.height, assets::encodeBc1(level)});
                    }
                    finished.blocks = std::move(blocks);
                } else {
                    finished.picture = std::move(*picture);
                }
                const std::lock_guard lock(shared->mutex);
                shared->finished.push_back(std::move(finished));
            },
            options);
        if (!submitted) {
            spdlog::warn("World: a gallery picture cannot be asked for: {}", submitted.error().toString());
            drawn->second.pending = false;
            continue;
        }
        drawn->second.submitted = true;
        ++inFlight;
    }
}

void WorldGallery::update(const Frame& frame) {
    if (frame.description == nullptr || frame.instance == nullptr || frame.materials == nullptr) {
        return;
    }
    if (m_dirty) {
        m_dirty = false;
        rescan(frame);
    }
    // At most kMaxInFlight pictures on the pool at once (all of them in a lockstep frame, which waits for them).
    pump(frame.settle);
    // A lockstep frame waits for every picture asked.
    if (frame.settle) {
        const auto waiting = [&] {
            std::size_t pending = 0;
            for (const auto& [request, drawn] : m_drawn) {
                pending += drawn.pending ? 1 : 0;
            }
            std::size_t arrived = 0;
            const std::lock_guard lock(m_shared->mutex);
            for (const Shared::Finished& done : m_shared->finished) {
                const auto drawn = m_drawn.find(done.request);
                arrived += drawn != m_drawn.end() && drawn->second.pending ? 1 : 0;
            }
            return arrived < pending;
        };
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(kSettleSeconds);
        while (waiting() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // The pictures made since, uploaded a few a frame (all of them in a lockstep frame).
    std::vector<Shared::Finished> finished;
    {
        const std::lock_guard lock(m_shared->mutex);
        const std::size_t take =
            frame.settle ? m_shared->finished.size() : std::min(kUploadsPerFrame, m_shared->finished.size());
        finished.assign(std::make_move_iterator(m_shared->finished.begin()),
                        std::make_move_iterator(m_shared->finished.begin() + static_cast<std::ptrdiff_t>(take)));
        m_shared->finished.erase(m_shared->finished.begin(),
                                 m_shared->finished.begin() + static_cast<std::ptrdiff_t>(take));
    }
    for (Shared::Finished& done : finished) {
        const auto drawn = m_drawn.find(done.request);
        if (drawn == m_drawn.end()) {
            continue; // no frame takes it any more
        }
        drawn->second.pending = false;
        if (!done.blocks && !done.picture) {
            m_failed.insert(done.request);
            spdlog::warn("World: a gallery frame keeps its print (its medium cannot be shown)");
            spdlog::debug("World: the gallery's medium {} cannot be shown: {}", done.request, done.error);
            continue;
        }
        gl::TextureDesc desc;
        desc.colorSpace = gl::ColorSpace::Srgb;
        desc.mipmaps = true;
        desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
        desc.sampling.filter = gl::TextureFilter::Trilinear;
        desc.memoryKind = gl::GpuMemoryKind::GalleryTextures;
        auto texture = done.blocks ? gl::Texture2D::createCompressed(*done.blocks, desc, "Gallery picture")
                                   : gl::Texture2D::create(*done.picture, desc, "Gallery picture");
        if (!texture) {
            m_failed.insert(done.request);
            spdlog::warn("World: a gallery picture cannot be uploaded: {}", texture.error().toString());
            continue;
        }
        drawn->second.texture = std::move(*texture);
    }
    // Each frame whose picture is there takes it.
    for (Target& target : m_targets) {
        if (target.applied) {
            continue;
        }
        const auto drawn = m_drawn.find(target.request);
        if (drawn == m_drawn.end() || !drawn->second.texture) {
            continue;
        }
        if (Status on = frame.materials->setTextureOverride(target.handle, gl::MaterialTextureSlot::BaseColor,
                                                            drawn->second.texture->id());
            !on) {
            spdlog::warn("World: the frame {} keeps its print: {}", target.objectId, on.error().toString());
            target.request.clear();
            continue;
        }
        target.applied = true;
    }
    m_peakBytes = std::max(m_peakBytes, bytes());
}

void WorldGallery::release(gl::MaterialRegistry* materials) {
    m_pool.cancel(m_generation);
    m_queue.clear();
    if (materials != nullptr) {
        takeOff(*materials);
    }
    m_targets.clear();
    m_drawn.clear();
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->finished.clear();
    }
    m_dirty = true;
}

} // namespace rv::app
