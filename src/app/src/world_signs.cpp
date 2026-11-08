#include "world_signs.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/app/scene_instance.hpp>
#include <rv/assets/svg_raster.hpp>
#include <rv/assets/text_raster.hpp>
#include <rv/core/utf8.hpp>

namespace rv::app {

namespace {

/// Behind the covers and the room's work, beside the UI's logos.
constexpr std::int64_t kJobPriority = -10;
/// Signs uploaded a frame (a few hundred microseconds each).
constexpr std::size_t kUploadsPerFrame = 2;

/// The material of `objectId`'s picture ("<id>/picture", "@<w>x<h>" for a pattern, then "#<hash>") with a base colour
/// texture of its own, from the object's render items (the registry is append-only: an object replaced in place leaves
/// its old material behind under the same prefix).
std::optional<scene::MaterialHandle> pictureMaterial(std::string_view objectId, const SceneInstance& instance,
                                                     const gl::MaterialRegistry& materials) {
    const std::string key = std::string(objectId) + "/" + std::string(kSignPictureRole);
    const std::span<const scene::RenderItem> items = instance.renderItems();
    for (const std::size_t item : instance.itemsOf([&](std::string_view id) { return id == objectId; })) {
        const scene::MaterialHandle handle = items[item].material;
        if (handle.value >= materials.size()) {
            continue;
        }
        const gl::GpuMaterial& material = materials.material(handle);
        if (material.key != key && !material.key.starts_with(key + "@") && !material.key.starts_with(key + "#")) {
            continue;
        }
        if (material.textureSources[gl::toIndex(gl::MaterialTextureSlot::BaseColor)] == nullptr) {
            continue;
        }
        return handle;
    }
    return std::nullopt;
}

/// A sign's request: its style, its size and what it shows, as one key (equal signs share a texture).
std::string requestKey(assets::IdentitySignStyle style, glm::ivec2 size, const IdentitySignContent& content) {
    std::string key = fmt::format("{}|{}x{}|{}", style == assets::IdentitySignStyle::Neon ? "neon" : "lit", size.x,
                                  size.y, content.name);
    for (const std::filesystem::path& logo : content.logos) {
        key += "|" + core::pathToUtf8Lossy(logo);
    }
    return key;
}

} // namespace

struct WorldSigns::Shared {
    struct Finished {
        std::string request;
        std::optional<assets::ImageData> picture;
        std::string error;
    };
    std::mutex mutex;
    std::vector<Finished> finished;
    /// The logos rasterized so far, by file (none: it could not be read), and the bold face, loaded once.
    std::map<std::filesystem::path, std::shared_ptr<const assets::ImageData>> logos;
    std::once_flag fontLoaded;
    std::optional<assets::FontFace> font;
    std::optional<Error> fontError;

    std::shared_ptr<const assets::ImageData> logo(const std::filesystem::path& file) {
        {
            const std::lock_guard lock(mutex);
            if (const auto found = logos.find(file); found != logos.end()) {
                return found->second;
            }
        }
        std::shared_ptr<const assets::ImageData> picture;
        if (auto loaded = assets::loadLogo(file, kSignLogoBoxWidth, kSignLogoBoxHeight)) {
            picture = std::make_shared<const assets::ImageData>(std::move(*loaded));
        } else {
            spdlog::warn("World: the logo {} is left off the signs: {}", core::pathToUtf8Lossy(file),
                         loaded.error().toString());
        }
        const std::lock_guard lock(mutex);
        return logos.emplace(file, std::move(picture)).first->second;
    }
};

WorldSigns::WorldSigns(core::WorkerPool& pool, std::filesystem::path assetRoot)
    : m_pool(pool), m_assetRoot(std::move(assetRoot)), m_shared(std::make_shared<Shared>()) {}

WorldSigns::~WorldSigns() {
    m_pool.cancel(m_generation);
}

std::size_t WorldSigns::shown() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(m_targets.begin(), m_targets.end(), [](const Target& target) { return target.applied; }));
}

void WorldSigns::takeOff(gl::MaterialRegistry& materials) {
    for (Target& target : m_targets) {
        if (!target.applied) {
            continue;
        }
        target.applied = false;
        // Only a material that is still the sign's (a registry cleared since holds others under its handles).
        if (target.handle.value < materials.size() && materials.material(target.handle).key == target.materialKey) {
            if (Status off = materials.setTextureOverride(target.handle, gl::MaterialTextureSlot::BaseColor, 0); !off) {
                spdlog::warn("World: the sign {} keeps its identity: {}", target.objectId, off.error().toString());
            }
        }
    }
}

void WorldSigns::rescan(const Frame& frame) {
    takeOff(*frame.materials);
    m_targets.clear();
    std::map<std::string, std::pair<assets::IdentitySignStyle, std::pair<glm::ivec2, IdentitySignContent>>,
             std::less<>>
        asks;
    for (const IdentitySignTarget& sign : identitySignTargets(*frame.description)) {
        const std::vector<catalog::SystemName> sections =
            frame.sections ? frame.sections(sign.room) : std::vector<catalog::SystemName>{};
        std::optional<IdentitySignContent> content =
            identitySignContent(sign.room, sign.place, sections, frame.systems);
        if (!content) {
            continue;
        }
        const std::optional<scene::MaterialHandle> handle =
            pictureMaterial(sign.objectId, *frame.instance, *frame.materials);
        if (!handle) {
            continue;
        }
        const gl::GpuMaterial& material = frame.materials->material(*handle);
        const gl::Texture2D* generic = material.textureSources[gl::toIndex(gl::MaterialTextureSlot::BaseColor)];
        const glm::ivec2 size = generic->size();
        if (size.x <= 0 || size.y <= 0) {
            continue;
        }
        std::string request = requestKey(sign.style, size, *content);
        m_targets.push_back(Target{sign.objectId, *handle, material.key, request, false});
        asks.try_emplace(std::move(request), sign.style, std::pair{size, std::move(*content)});
    }
    // Textures no sign takes any more are released (their overrides are off); new requests go to the pool.
    for (auto drawn = m_drawn.begin(); drawn != m_drawn.end();) {
        drawn = asks.contains(drawn->first) ? std::next(drawn) : m_drawn.erase(drawn); // a late result is dropped
    }
    for (auto& [request, ask] : asks) {
        if (m_drawn.contains(request)) {
            continue;
        }
        m_drawn.emplace(request, Drawn{std::nullopt, false, true});
        core::JobOptions options;
        options.priority = kJobPriority;
        options.generation = &m_generation;
        const Status submitted = m_pool.submit(
            [shared = m_shared, assetRoot = m_assetRoot, request = request, style = ask.first, size = ask.second.first,
             content = ask.second.second](const core::JobContext& job) {
                if (job.cancelled()) {
                    return;
                }
                Shared::Finished finished{request, std::nullopt, {}};
                std::call_once(shared->fontLoaded, [&] {
                    if (auto font = assets::FontFace::loadBundled(assetRoot, assets::kLabelTitleFontPath)) {
                        shared->font = std::move(*font);
                    } else {
                        shared->fontError = font.error();
                    }
                });
                if (shared->fontError) {
                    finished.error = shared->fontError->toString();
                } else {
                    std::vector<assets::ImageData> logos;
                    for (const std::filesystem::path& file : content.logos) {
                        if (std::shared_ptr<const assets::ImageData> logo = shared->logo(file)) {
                            logos.push_back(*logo);
                        }
                    }
                    if (logos.empty() && content.name.empty()) {
                        finished.error = "none of its logos can be read";
                    } else {
                        finished.picture =
                            assets::makeIdentitySign(style, size.x, size.y, logos, content.name, *shared->font);
                    }
                }
                const std::lock_guard lock(shared->mutex);
                shared->finished.push_back(std::move(finished));
            },
            options);
        if (!submitted) {
            spdlog::warn("World: a sign cannot be asked for: {}", submitted.error().toString());
            m_drawn[request].failed = true;
            m_drawn[request].pending = false;
        }
    }
}

void WorldSigns::update(const Frame& frame) {
    if (frame.description == nullptr || frame.instance == nullptr || frame.materials == nullptr) {
        return;
    }
    if (m_dirty || m_systemsRevision != frame.systemsRevision) {
        m_dirty = false;
        m_systemsRevision = frame.systemsRevision;
        rescan(frame);
    }
    // A lockstep frame waits for every sign asked (the jobs are milliseconds each).
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
    // The signs drawn since, uploaded a few a frame (all of them in a lockstep frame).
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
            continue; // no sign takes it any more
        }
        drawn->second.pending = false;
        if (!done.picture || !done.picture->isValid()) {
            ++m_failed;
            drawn->second.failed = true;
            spdlog::warn("World: a sign keeps its generic picture ({}): {}", done.request, done.error);
            continue;
        }
        gl::TextureDesc desc;
        desc.colorSpace = gl::ColorSpace::Srgb;
        desc.mipmaps = true;
        desc.sampling.wrap = gl::TextureWrap::ClampToEdge;
        desc.sampling.filter = gl::TextureFilter::Trilinear;
        desc.memoryKind = gl::GpuMemoryKind::SignTextures;
        auto texture = gl::Texture2D::create(*done.picture, desc, "Identity sign");
        if (!texture) {
            ++m_failed;
            drawn->second.failed = true;
            spdlog::warn("World: a sign cannot be uploaded: {}", texture.error().toString());
            continue;
        }
        drawn->second.texture = std::move(*texture);
    }
    // Each sign whose texture is there takes it.
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
            spdlog::warn("World: the sign {} keeps its generic picture: {}", target.objectId, on.error().toString());
            target.request.clear();
            continue;
        }
        target.applied = true;
    }
}

void WorldSigns::release(gl::MaterialRegistry* materials) {
    m_pool.cancel(m_generation);
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
    m_systemsRevision.reset();
}

} // namespace rv::app
