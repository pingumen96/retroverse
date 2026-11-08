#pragma once

// The rooms' signs drawn from the systems' identity in the scene shown (I11 step 4, ADR-0019 §3). The scene's signs
// (identitySignTargets()) keep their generic pictures in the scene file; for each one whose room has an identity
// (identitySignContent()) the sign is drawn on the worker pool (assets::makeIdentitySign(): the logos rasterized once
// per file, the bundled bold face), uploaded on the main thread, two a frame, and put on its picture's material as a
// texture override (gl::MaterialRegistry::setTextureOverride), at the size of the generic picture it replaces. Equal
// signs (a portal's two blades) share one texture. Without an identity nothing changes: every scene without a theme
// draws as before.
//
// The signs are found again when the scene changes (sceneChanged(): a floor shown, objects replaced) or the catalog's
// systems do (their revision). A logo or a sign that cannot be drawn is logged and leaves that sign generic.
//
// I11 step 6 (ADR-0019 §5): with "Colori dei sistemi" on (setSystemColors()), a console room whose system has an accent
// (SignSystem::accent) has its neon in that colour: the neon's picture materials (the room's sign, the hub's fascia
// over its doorway when it takes the neon) get it as their base colour (gl::MaterialRegistry::setFactors), the neon's
// glow light ("<room>/accent:sign_glow") takes it and the sign's picture light ("<room>/fx:hero.sign-light") its tint
// (world::signLightTint()) in place of the pack's. Off, or taken off, every material and light is as it was.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/identity_signs.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/material_uniforms.hpp>
#include <rv/render/texture.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

class SceneInstance;

/// The box each logo file is rasterized into once (the signs resample it down: the largest is 512 pixels wide).
inline constexpr int kSignLogoBoxWidth = 1024;
inline constexpr int kSignLogoBoxHeight = 256;

class WorldSigns {
public:
    /// What a frame gives: the scene shown, its materials, each room's sections (the plan's) and each system as the
    /// catalog has it, with the revision of the catalog's systems.
    struct Frame {
        const scene::SceneDescription* description = nullptr;
        SceneInstance* instance = nullptr;
        gl::MaterialRegistry* materials = nullptr;
        std::function<std::vector<catalog::SystemName>(const world::RoomId&)> sections;
        SignSystemLookup systems;
        std::uint64_t systemsRevision = 0;
        /// A lockstep run's frame (I3 step 12's rule): the signs asked are waited for (at most kSettleSeconds) and all
        /// put on at once, so that its pictures never depend on the pool's timing.
        bool settle = false;
    };
    static constexpr double kSettleSeconds = 5.0;

    WorldSigns(core::WorkerPool& pool, std::filesystem::path assetRoot);
    ~WorldSigns();
    WorldSigns(const WorldSigns&) = delete;
    WorldSigns& operator=(const WorldSigns&) = delete;

    /// The scene was replaced or some of its objects were: its signs are found again on the next update().
    void sceneChanged() noexcept { m_dirty = true; }
    /// I11 step 6: the settings' "Colori dei sistemi" (world.systemColors); a change applies on the next update().
    void setSystemColors(bool on) noexcept {
        if (on != m_systemColors) {
            m_systemColors = on;
            m_dirty = true;
        }
    }
    [[nodiscard]] bool systemColors() const noexcept { return m_systemColors; }
    /// Once a frame, with the scene shown (none: nothing to do).
    void update(const Frame& frame);
    /// Every override taken off `materials` (when they still hold them), the textures released, the jobs cancelled
    /// (before the pool or the renderer goes).
    void release(gl::MaterialRegistry* materials, SceneInstance* instance = nullptr);

    /// The signs of the scene that show an identity (an override applied), those that should, and the signs that
    /// could not be drawn since the run began.
    [[nodiscard]] std::size_t shown() const noexcept;
    [[nodiscard]] std::size_t wanted() const noexcept { return m_targets.size(); }
    [[nodiscard]] std::size_t failed() const noexcept { return m_failed; }
    /// I11 step 6: the neon materials and the lights in their systems' colours now.
    [[nodiscard]] std::size_t tintedMaterials() const noexcept { return m_tints.size(); }
    [[nodiscard]] std::size_t tintedLights() const noexcept { return m_lightTints.size(); }

private:
    struct Shared;
    /// A sign of the scene with an identity: its picture's material, and the texture it takes.
    struct Target {
        std::string objectId;
        scene::MaterialHandle handle;
        std::string materialKey;
        std::string request;
        bool applied = false;
    };
    /// A texture drawn for one or more signs, by its request.
    struct Drawn {
        std::optional<gl::Texture2D> texture;
        bool failed = false;
        /// With the pool: neither drawn nor failed yet.
        bool pending = false;
    };

    /// A neon material in its system's colour, and what it was.
    struct Tint {
        scene::MaterialHandle handle;
        std::string materialKey;
        gl::MaterialFactors original;
    };
    /// A light in its system's colour, and its colour before.
    struct LightTint {
        std::size_t index = 0;
        std::string name;
        glm::vec3 original{1.0F};
    };

    void rescan(const Frame& frame);
    void takeOff(gl::MaterialRegistry& materials, SceneInstance* instance);
    void tint(const Frame& frame);

    core::WorkerPool& m_pool;
    std::filesystem::path m_assetRoot;
    std::shared_ptr<Shared> m_shared;
    core::JobGeneration m_generation;
    bool m_dirty = true;
    std::optional<std::uint64_t> m_systemsRevision;
    std::vector<Target> m_targets;
    std::map<std::string, Drawn, std::less<>> m_drawn;
    std::size_t m_failed = 0;
    bool m_systemColors = true;
    std::vector<Tint> m_tints;
    std::vector<LightTint> m_lightTints;
};

} // namespace rv::app
