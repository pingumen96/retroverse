#include "world_consoles.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

namespace rv::app {

namespace {

constexpr std::int64_t kJobPriority = -10;

/// A stand-in for an image whose compressed blocks replace it on the GPU: 1 x 1, its channels kept.
assets::ImageData standIn(const assets::ImageData& image) {
    assets::ImageData small;
    small.width = 1;
    small.height = 1;
    small.channels = std::max(1, image.channels);
    small.pixels.assign(static_cast<std::size_t>(small.channels), 255);
    return small;
}

} // namespace

struct WorldConsoles::Shared {
    struct Finished {
        std::string system;
        std::optional<assets::ConsoleContent> content;
        assets::ConsoleCheck check;
        std::vector<assets::CompressedTexture> blocks;
        std::string error;
    };
    std::mutex mutex;
    std::vector<Finished> finished;
};

WorldConsoles::WorldConsoles(core::WorkerPool& pool) : m_pool(pool), m_shared(std::make_shared<Shared>()) {}

WorldConsoles::~WorldConsoles() {
    m_pool.cancel(m_generation);
}

std::size_t WorldConsoles::refused() const noexcept {
    return static_cast<std::size_t>(std::count_if(m_loaded.begin(), m_loaded.end(),
                                                  [](const auto& entry) { return entry.second.refused; }));
}

std::size_t WorldConsoles::textureBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& [system, loaded] : m_loaded) {
        for (const auto& [key, texture] : loaded.textures) {
            bytes += texture.estimatedBytes();
        }
    }
    return bytes;
}

Expected<std::shared_ptr<const assets::ModelData>> WorldConsoles::model(std::string_view modelPath) const {
    for (const auto& [system, loaded] : m_loaded) {
        if (loaded.modelPath == modelPath && loaded.model) {
            return loaded.model;
        }
    }
    return makeError(ErrorCode::NotFound, "no console model loaded under this path", std::string(modelPath));
}

void WorldConsoles::ask(const std::string& system, const std::filesystem::path& folder, bool compress) {
    m_loaded[system] = Loaded{};
    core::JobOptions options;
    options.priority = kJobPriority;
    options.generation = &m_generation;
    const Status submitted = m_pool.submit(
        [shared = m_shared, system, folder, compress](const core::JobContext& job) {
            if (job.cancelled()) {
                return;
            }
            Shared::Finished finished;
            finished.system = system;
            if (auto content = assets::loadConsoleContent(folder)) {
                finished.check = assets::checkConsoleModel(content->model, content->manifest);
                if (finished.check.ok() && compress) {
                    finished.blocks = assets::compressConsoleTextures(content->model);
                    // The GPU takes the blocks: the images uploaded with the model are 1 x 1 stand-ins.
                    for (const assets::CompressedTexture& texture : finished.blocks) {
                        assets::ImageData& image = content->model.images[texture.image];
                        if (image.width > 1 || image.height > 1) {
                            image = standIn(image);
                        }
                    }
                }
                finished.content = std::move(*content);
            } else {
                finished.error = content.error().toString();
            }
            const std::lock_guard lock(shared->mutex);
            shared->finished.push_back(std::move(finished));
        },
        options);
    if (!submitted) {
        m_loaded[system].pending = false;
        m_loaded[system].refused = true;
        spdlog::warn("World: the console model of {} cannot be asked for: {}", system, submitted.error().toString());
    }
}

void WorldConsoles::rescan(const Frame& frame) {
    m_sites = consoleSites(*frame.description);
    m_shown.clear();
    for (const ConsoleSite& site : m_sites) {
        const std::string system = site.room.system().str();
        if (m_loaded.contains(system)) {
            continue;
        }
        const std::filesystem::path folder = assets::consoleFolder(frame.contentDir, system);
        std::error_code error;
        if (!std::filesystem::exists(folder / core::pathFromUtf8(assets::kConsoleManifestName), error)) {
            continue;
        }
        ask(system, folder, frame.compressed);
    }
}

void WorldConsoles::collect(bool settle) {
    if (settle) {
        const auto waiting = [&] {
            std::size_t pending = 0;
            for (const auto& [system, loaded] : m_loaded) {
                pending += loaded.pending ? 1 : 0;
            }
            const std::lock_guard lock(m_shared->mutex);
            return m_shared->finished.size() < pending;
        };
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(kSettleSeconds);
        while (waiting() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    std::vector<Shared::Finished> finished;
    {
        const std::lock_guard lock(m_shared->mutex);
        finished.swap(m_shared->finished);
    }
    for (Shared::Finished& done : finished) {
        const auto found = m_loaded.find(done.system);
        if (found == m_loaded.end() || !found->second.pending) {
            continue;
        }
        Loaded& loaded = found->second;
        loaded.pending = false;
        if (!done.content) {
            loaded.refused = true;
            spdlog::warn("World: the console model of {} cannot be read; its room keeps the procedural console: {}",
                         done.system, done.error);
            continue;
        }
        loaded.check = done.check;
        if (!done.check.ok()) {
            loaded.refused = true;
            std::string problems;
            for (const std::string& problem : done.check.problems) {
                problems += (problems.empty() ? "" : "; ") + problem;
            }
            spdlog::warn("World: the console model of {} is refused; its room keeps the procedural console: {}",
                         done.system, problems);
            continue;
        }
        loaded.manifest = done.content->manifest;
        loaded.modelPath = contentModelPath(done.system, loaded.manifest.model);
        loaded.model = std::make_shared<const assets::ModelData>(std::move(done.content->model));
        loaded.blocks = std::move(done.blocks);
        spdlog::info("World: the console model of {}: {:.1f} x {:.1f} x {:.1f} cm, {} triangle(s), {:.2f} MiB of "
                     "textures compressed",
                     done.system, done.check.size.x * 100.0F, done.check.size.y * 100.0F, done.check.size.z * 100.0F,
                     done.check.triangles, static_cast<double>(done.check.textureBytes) / (1024.0 * 1024.0));
    }
}

void WorldConsoles::overrideTextures(const Frame& frame, Loaded& loaded) {
    if (!loaded.uploaded) {
        loaded.uploaded = true;
        for (const assets::CompressedTexture& blocks : loaded.blocks) {
            gl::TextureDesc desc;
            desc.memoryKind = gl::GpuMemoryKind::ContentTextures;
            desc.sampling.wrap = gl::TextureWrap::Repeat;
            desc.sampling.filter = gl::TextureFilter::Trilinear;
            auto texture = gl::Texture2D::createCompressed(
                blocks, desc, fmt::format("{}#image{}", loaded.modelPath, blocks.image));
            if (!texture) {
                spdlog::warn("World: a texture of {} cannot be uploaded: {}", loaded.modelPath,
                             texture.error().toString());
                continue;
            }
            loaded.textures.emplace(std::pair{blocks.image, blocks.srgb}, std::move(*texture));
        }
    }
    // Each material of the model (GpuModel registers "<model>#material<i>") takes the blocks of the images it samples.
    for (std::size_t index = 0; index < loaded.model->materials.size(); ++index) {
        const std::optional<scene::MaterialHandle> handle =
            frame.materials->find(fmt::format("{}#material{}", loaded.modelPath, index));
        if (!handle) {
            continue;
        }
        const assets::MaterialDesc& material = loaded.model->materials[index];
        const std::array<std::tuple<const assets::TextureRef*, gl::MaterialTextureSlot, bool>, 5> slots{{
            {&material.baseColorTexture, gl::MaterialTextureSlot::BaseColor, true},
            {&material.metallicRoughnessTexture, gl::MaterialTextureSlot::MetallicRoughness, false},
            {&material.normalTexture, gl::MaterialTextureSlot::Normal, false},
            {&material.occlusionTexture, gl::MaterialTextureSlot::Occlusion, false},
            {&material.emissiveTexture, gl::MaterialTextureSlot::Emissive, true},
        }};
        for (const auto& [ref, slot, srgb] : slots) {
            if (!ref->imageIndex) {
                continue;
            }
            const auto texture = loaded.textures.find(std::pair{*ref->imageIndex, srgb});
            if (texture == loaded.textures.end()) {
                continue;
            }
            if (Status on = frame.materials->setTextureOverride(*handle, slot, texture->second.id()); !on) {
                spdlog::warn("World: {} keeps a stand-in texture: {}", loaded.modelPath, on.error().toString());
            }
        }
    }
}

void WorldConsoles::swap(const Frame& frame) {
    for (const ConsoleSite& site : m_sites) {
        if (std::find(m_shown.begin(), m_shown.end(), site.objectId) != m_shown.end()) {
            continue;
        }
        const auto found = m_loaded.find(site.room.system().str());
        if (found == m_loaded.end() || found->second.pending || found->second.refused || !found->second.model) {
            continue;
        }
        Loaded& loaded = found->second;
        const float width = loaded.check.size.x;
        const glm::mat4 local = consoleModelLocal(*loaded.model, loaded.manifest, width);
        SceneObjectsChange change;
        change.prefix = site.objectId;
        change.objects.push_back(consoleModelObject(site, loaded.modelPath, local));
        if (!frame.replaceObjects || !frame.replaceObjects(change)) {
            spdlog::warn("World: the console model of {} cannot be put in the scene", site.room.toString());
            continue;
        }
        m_shown.push_back(site.objectId);
        overrideTextures(frame, loaded);
        // The insertion goes into the model's socket; the CRT's screen stays the room's.
        if (frame.setConsoleSocket) {
            std::optional<glm::mat4> heroScreen;
            if (frame.consoleSocket) {
                if (const std::optional<ConsoleSocket> before = frame.consoleSocket(site.room)) {
                    heroScreen = before->heroScreen;
                }
            }
            frame.setConsoleSocket(site.room, consoleModelSocket(*loaded.model, loaded.manifest, site, width, heroScreen));
        }
        spdlog::info("World: {} shows its console model", site.room.toString());
    }
}

void WorldConsoles::update(const Frame& frame) {
    if (frame.description == nullptr || frame.instance == nullptr || frame.materials == nullptr) {
        return;
    }
    if (m_dirty) {
        m_dirty = false;
        rescan(frame);
    }
    collect(frame.settle);
    if (!frame.frozen) {
        swap(frame);
    }
}

void WorldConsoles::release() {
    m_pool.cancel(m_generation);
    m_loaded.clear();
    m_sites.clear();
    m_shown.clear();
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->finished.clear();
    }
    m_dirty = true;
}

} // namespace rv::app
