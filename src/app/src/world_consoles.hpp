#pragma once

// The user's console models in the scene shown (I11 step 7, ADR-0019 §7). For each console room's procedural hero
// console (consoleSites()) whose system has a folder in <user data>/content/consoles, the model is read, checked
// (assets::checkConsoleModel()) and its textures compressed (BC1, BC3) on the worker pool, once per system and run.
// A model that passes takes the console's place through the host's replaceObjects() under the console's own id (the
// scene's model loader resolves "content:" paths with model()), its textures go on its materials as S3TC overrides,
// and its media socket becomes the room's (the insertion). One that fails is logged once and never shown: the room keeps
// its procedural console. Without content nothing is replaced.
//
// The consoles are found again when the scene changes (sceneChanged(): a floor shown, a room rebuilt, which brings the
// procedural console back); while `frozen` (the room editor) nothing is replaced. A lockstep run waits for its models.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/content_consoles.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/assets/console_content.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/texture.hpp>

namespace rv::app {

class WorldConsoles {
public:
    struct Frame {
        scene::SceneDescription* description = nullptr;
        SceneInstance* instance = nullptr;
        gl::MaterialRegistry* materials = nullptr;
        /// <user data>/content.
        std::filesystem::path contentDir;
        /// The driver takes S3TC sRGB textures (GlDevice::s3tcSrgb()): else the models keep their RGBA8 textures.
        bool compressed = true;
        bool settle = false;
        bool frozen = false;
        std::function<bool(const SceneObjectsChange&)> replaceObjects;
        /// The room's console socket now, and the model's in its place.
        std::function<std::optional<ConsoleSocket>(const world::RoomId&)> consoleSocket;
        std::function<void(const world::RoomId&, std::optional<ConsoleSocket>)> setConsoleSocket;
    };
    static constexpr double kSettleSeconds = 20.0;

    explicit WorldConsoles(core::WorkerPool& pool);
    ~WorldConsoles();
    WorldConsoles(const WorldConsoles&) = delete;
    WorldConsoles& operator=(const WorldConsoles&) = delete;

    void sceneChanged() noexcept { m_dirty = true; }
    void update(const Frame& frame);
    /// The textures released, the jobs cancelled (the models stay in their scene).
    void release();

    /// The model of a "content:" path once loaded and checked (its images the stand-ins its textures replace); an error
    /// for another path or a model not loaded.
    [[nodiscard]] Expected<std::shared_ptr<const assets::ModelData>> model(std::string_view modelPath) const;

    /// The consoles of the scene drawn from a model now, the models refused since the run began, and their
    /// compressed textures' bytes.
    [[nodiscard]] std::size_t shown() const noexcept { return m_shown.size(); }
    [[nodiscard]] std::size_t refused() const noexcept;
    [[nodiscard]] std::size_t textureBytes() const noexcept;

private:
    struct Shared;
    /// A system's console, as the pool read it.
    struct Loaded {
        bool pending = true;
        bool refused = false;
        std::string modelPath;
        std::shared_ptr<const assets::ModelData> model;
        assets::ConsoleManifest manifest;
        assets::ConsoleCheck check;
        std::vector<assets::CompressedTexture> blocks;
        /// Uploaded once, kept while the run lasts: by (image, sRGB).
        std::map<std::pair<std::uint32_t, bool>, gl::Texture2D> textures;
        bool uploaded = false;
    };

    void rescan(const Frame& frame);
    void ask(const std::string& system, const std::filesystem::path& folder, bool compress);
    void collect(bool settle);
    void swap(const Frame& frame);
    void overrideTextures(const Frame& frame, Loaded& loaded);

    core::WorkerPool& m_pool;
    std::shared_ptr<Shared> m_shared;
    core::JobGeneration m_generation;
    bool m_dirty = true;
    /// By system.
    std::map<std::string, Loaded, std::less<>> m_loaded;
    /// The scene's console sites, and those its models stand in now.
    std::vector<ConsoleSite> m_sites;
    std::vector<std::string> m_shown;
};

} // namespace rv::app
