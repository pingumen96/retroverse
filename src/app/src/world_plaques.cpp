#include "world_plaques.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/assets/svg_raster.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/core/utf8.hpp>

#include "world_signs.hpp"

namespace rv::app {

namespace {

constexpr std::int64_t kJobPriority = -10;

} // namespace

struct WorldPlaques::Shared {
    struct Finished {
        std::string room;
        std::optional<assets::ImageData> picture;
        std::string error;
    };
    std::mutex mutex;
    std::vector<Finished> finished;
    std::once_flag fontsLoaded;
    std::optional<assets::FontFace> bold;
    std::optional<assets::FontFace> regular;
    std::optional<Error> fontError;
};

WorldPlaques::WorldPlaques(core::WorkerPool& pool, std::filesystem::path assetRoot)
    : m_pool(pool), m_assetRoot(std::move(assetRoot)), m_shared(std::make_shared<Shared>()) {}

WorldPlaques::~WorldPlaques() {
    m_pool.cancel(m_generation);
}

std::size_t WorldPlaques::drawn() const noexcept {
    return static_cast<std::size_t>(std::count_if(m_plaques.begin(), m_plaques.end(),
                                                  [](const auto& entry) { return entry.second.applied; }));
}

void WorldPlaques::takeOff(gl::MaterialRegistry& materials) {
    for (auto& [room, plaque] : m_plaques) {
        if (plaque.applied && plaque.handle && plaque.handle->value < materials.size() &&
            materials.material(*plaque.handle).key == plaque.materialKey) {
            static_cast<void>(materials.setTextureOverride(*plaque.handle, gl::MaterialTextureSlot::BaseColor, 0));
        }
        plaque.applied = false;
    }
}

void WorldPlaques::ask(Plaque& plaque) {
    plaque.pending = true;
    core::JobOptions options;
    options.priority = kJobPriority;
    options.generation = &m_generation;
    const Status submitted = m_pool.submit(
        [shared = m_shared, assetRoot = m_assetRoot, room = plaque.site.room.toString(), text = plaque.text,
         logo = plaque.logo](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            Shared::Finished finished{room, std::nullopt, {}};
            std::call_once(shared->fontsLoaded, [&] {
                auto bold = assets::FontFace::loadBundled(assetRoot, assets::kLabelTitleFontPath);
                auto regular = assets::FontFace::loadBundled(assetRoot, assets::kLabelDetailFontPath);
                if (bold && regular) {
                    shared->bold = std::move(*bold);
                    shared->regular = std::move(*regular);
                } else {
                    shared->fontError = bold ? regular.error() : bold.error();
                }
            });
            if (shared->fontError) {
                finished.error = shared->fontError->toString();
            } else {
                std::optional<assets::ImageData> picture;
                if (!logo.empty()) {
                    if (auto loaded = assets::loadLogo(logo, kSignLogoBoxWidth, kSignLogoBoxHeight)) {
                        picture = std::move(*loaded);
                    }
                }
                finished.picture =
                    assets::makeHistoryPlaque(kPlaquePictureWidth, kPlaquePictureHeight, text,
                                              picture ? &*picture : nullptr, *shared->bold, *shared->regular);
            }
            const std::lock_guard lock(shared->mutex);
            shared->finished.push_back(std::move(finished));
        },
        options);
    if (!submitted) {
        plaque.pending = false;
        plaque.failed = true;
        ++m_failed;
        spdlog::warn("World: the plaque of {} cannot be asked for: {}", plaque.site.room.toString(),
                     submitted.error().toString());
    }
}

void WorldPlaques::rescan(const Frame& frame) {
    // What the scene should hold: a plaque per console room whose system has a description.
    std::vector<PlaqueSite> sites;
    std::map<std::string, Plaque, std::less<>> wanted;
    for (const PlaqueSite& site : plaqueSites(*frame.description)) {
        const std::optional<SignSystem> system = frame.systems ? frame.systems(site.room.system()) : std::nullopt;
        if (!system) {
            continue;
        }
        std::optional<assets::PlaqueText> text = plaqueText(*system);
        if (!text) {
            continue;
        }
        Plaque plaque;
        plaque.site = site;
        plaque.system = site.room.system().str();
        plaque.text = std::move(*text);
        plaque.logo = system->logo;
        wanted.emplace(site.room.toString(), std::move(plaque));
        sites.push_back(site);
    }
    // What it holds.
    std::vector<std::string> present;
    for (const scene::SceneObject& object : frame.description->objects) {
        if (object.id.starts_with(kPlaquePrefix)) {
            present.push_back(object.id);
        }
    }
    std::vector<std::string> wantedIds;
    for (const PlaqueSite& site : sites) {
        wantedIds.push_back(plaqueId(site.room));
    }
    const bool same = present == wantedIds &&
                      std::all_of(sites.begin(), sites.end(), [&](const PlaqueSite& site) {
                          const scene::SceneObject* object = frame.description->findObject(plaqueId(site.room));
                          return object != nullptr && object->transform == site.transform;
                      });
    if (!same) {
        if (frame.frozen || !frame.replaceObjects) {
            m_dirty = true; // again once the editor lets go
            return;
        }
        SceneObjectsChange change;
        change.prefix = std::string(kPlaquePrefix);
        for (const PlaqueSite& site : sites) {
            change.objects.push_back(plaqueObject(site));
        }
        takeOff(*frame.materials);
        if (!frame.replaceObjects(change)) {
            spdlog::warn("World: the rooms' plaques cannot be put in the scene");
            m_plaques.clear();
            return;
        }
        if (!sites.empty() || !present.empty()) {
            spdlog::info("World: {} history plaque(s) beside the hero corners", sites.size());
        }
    }
    // The plates: kept when their text and logo are the same, else drawn again.
    takeOff(*frame.materials);
    for (auto& [room, plaque] : wanted) {
        plaque.handle = objectPictureMaterial(plaqueId(plaque.site.room), *frame.instance, *frame.materials);
        if (plaque.handle) {
            plaque.materialKey = frame.materials->material(*plaque.handle).key;
        }
        const auto old = m_plaques.find(room);
        if (old != m_plaques.end() && old->second.text == plaque.text && old->second.logo == plaque.logo &&
            (old->second.texture || old->second.pending)) {
            plaque.texture = std::move(old->second.texture);
            plaque.pending = old->second.pending;
        }
    }
    m_plaques = std::move(wanted);
    for (auto& [room, plaque] : m_plaques) {
        if (!plaque.texture && !plaque.pending && plaque.handle) {
            ask(plaque);
        }
    }
}

void WorldPlaques::update(const Frame& frame) {
    if (frame.description == nullptr || frame.instance == nullptr || frame.materials == nullptr) {
        return;
    }
    if (m_dirty || m_systemsRevision != frame.systemsRevision) {
        m_dirty = false;
        m_systemsRevision = frame.systemsRevision;
        rescan(frame);
    }
    if (frame.settle) {
        const auto waiting = [&] {
            std::size_t pending = 0;
            for (const auto& [room, plaque] : m_plaques) {
                pending += plaque.pending ? 1 : 0;
            }
            std::size_t arrived = 0;
            const std::lock_guard lock(m_shared->mutex);
            for (const Shared::Finished& done : m_shared->finished) {
                const auto plaque = m_plaques.find(done.room);
                arrived += plaque != m_plaques.end() && plaque->second.pending ? 1 : 0;
            }
            return arrived < pending;
        };
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(kSettleSeconds);
        while (waiting() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::vector<Shared::Finished> finished;
    {
        const std::lock_guard lock(m_shared->mutex);
        const std::size_t take = frame.settle ? m_shared->finished.size() : std::min<std::size_t>(1, m_shared->finished.size());
        finished.assign(std::make_move_iterator(m_shared->finished.begin()),
                        std::make_move_iterator(m_shared->finished.begin() + static_cast<std::ptrdiff_t>(take)));
        m_shared->finished.erase(m_shared->finished.begin(),
                                 m_shared->finished.begin() + static_cast<std::ptrdiff_t>(take));
    }
    for (Shared::Finished& done : finished) {
        const auto found = m_plaques.find(done.room);
        if (found == m_plaques.end() || !found->second.pending) {
            continue;
        }
        Plaque& plaque = found->second;
        plaque.pending = false;
        if (!done.picture || !done.picture->isValid()) {
            plaque.failed = true;
            ++m_failed;
            spdlog::warn("World: the plaque of {} is not drawn: {}", done.room, done.error);
            continue;
        }
        gl::TextureDesc desc;
        desc.colorSpace = gl::ColorSpace::Srgb;
        desc.mipmaps = true;
        desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
        desc.sampling.filter = gl::TextureFilter::Trilinear;
        desc.memoryKind = gl::GpuMemoryKind::SignTextures;
        auto texture = gl::Texture2D::create(*done.picture, desc, "History plaque");
        if (!texture) {
            plaque.failed = true;
            ++m_failed;
            spdlog::warn("World: the plaque of {} cannot be uploaded: {}", done.room, texture.error().toString());
            continue;
        }
        plaque.texture = std::move(*texture);
    }
    for (auto& [room, plaque] : m_plaques) {
        if (plaque.applied || !plaque.texture || !plaque.handle) {
            continue;
        }
        if (Status on = frame.materials->setTextureOverride(*plaque.handle, gl::MaterialTextureSlot::BaseColor,
                                                            plaque.texture->id());
            !on) {
            spdlog::warn("World: the plaque of {} keeps a blank plate: {}", room, on.error().toString());
            plaque.handle.reset();
            continue;
        }
        plaque.applied = true;
    }
}

void WorldPlaques::release(gl::MaterialRegistry* materials) {
    m_pool.cancel(m_generation);
    if (materials != nullptr) {
        takeOff(*materials);
    }
    m_plaques.clear();
    m_hovered.reset();
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->finished.clear();
    }
    m_dirty = true;
    m_systemsRevision.reset();
}

const std::optional<world::RoomId>& WorldPlaques::hover(const std::optional<core::Ray>& ray) {
    m_hovered.reset();
    if (!ray) {
        return m_hovered;
    }
    std::optional<float> nearest;
    for (const auto& [room, plaque] : m_plaques) {
        if (const std::optional<float> hit = plaqueHit(plaque.site.transform, *ray); hit && (!nearest || *hit < *nearest)) {
            nearest = hit;
            m_hovered = plaque.site.room;
        }
    }
    return m_hovered;
}

std::optional<std::pair<core::Aabb, std::string>> WorldPlaques::hoveredLabel() const {
    if (!m_hovered) {
        return std::nullopt;
    }
    const auto found = m_plaques.find(m_hovered->toString());
    if (found == m_plaques.end()) {
        return std::nullopt;
    }
    return std::pair{plaqueBounds(found->second.site.transform), found->second.text.name};
}

std::optional<PlaqueCardContent>
WorldPlaques::hoveredCard(const std::function<std::string(const std::string&)>& logoSource) const {
    if (!m_hovered) {
        return std::nullopt;
    }
    const auto found = m_plaques.find(m_hovered->toString());
    if (found == m_plaques.end()) {
        return std::nullopt;
    }
    const Plaque& plaque = found->second;
    return PlaqueCardContent{plaque.system,    plaque.text.name,        plaque.text.maker,
                             plaque.text.year, plaque.text.type,        plaque.text.description,
                             logoSource ? logoSource(plaque.system) : std::string()};
}

} // namespace rv::app
