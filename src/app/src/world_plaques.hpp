#pragma once

// The console rooms' history plaques in the scene shown (I11 step 5, ADR-0019 §6). For each console room whose system's
// identity has a description (plaqueText()), a plaque object stands beside its hero corner (plaqueSites(),
// plaqueObject()), added to the scene at runtime under kPlaquePrefix through the host's replaceObjects(); its plate is
// drawn on the worker pool (assets::makeHistoryPlaque(): the logo engraved, the bundled faces) and put on the plaque's
// picture as a texture override, as the identity's signs are (WorldSigns). Without such a room nothing is added and the
// scene stays as the plan built it.
//
// The plaques are found again when the scene changes (sceneChanged()) or the catalog's systems do; while `frozen` (the
// room editor holds render items by index) nothing is added or taken away. hover() follows the frame's pick ray: the
// plaque it points at, for the hover's label and interact's card (PlaqueCardPanel).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/history_plaques.hpp>
#include <rv/app/plaque_card_ui.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/texture.hpp>

namespace rv::app {

class WorldPlaques {
public:
    struct Frame {
        scene::SceneDescription* description = nullptr;
        SceneInstance* instance = nullptr;
        gl::MaterialRegistry* materials = nullptr;
        SignSystemLookup systems;
        std::uint64_t systemsRevision = 0;
        /// A lockstep frame: the plates asked are waited for (at most kSettleSeconds) and put on at once.
        bool settle = false;
        /// The room editor holds render items by index: no object added or taken away now.
        bool frozen = false;
        /// Replaces the scene's objects under a prefix (SceneInstance::replaceObjects() and the walker's collision);
        /// false when it could not.
        std::function<bool(const SceneObjectsChange&)> replaceObjects;
    };
    static constexpr double kSettleSeconds = 5.0;

    WorldPlaques(core::WorkerPool& pool, std::filesystem::path assetRoot);
    ~WorldPlaques();
    WorldPlaques(const WorldPlaques&) = delete;
    WorldPlaques& operator=(const WorldPlaques&) = delete;

    void sceneChanged() noexcept { m_dirty = true; }
    /// Once a frame, with the scene shown.
    void update(const Frame& frame);
    /// The overrides taken off, the textures released, the jobs cancelled (the objects stay with their scene).
    void release(gl::MaterialRegistry* materials);

    /// The plaque `ray` points at (the nearest within kPlaquePickDistance), none without a ray; remembered as hovered().
    const std::optional<world::RoomId>& hover(const std::optional<core::Ray>& ray);
    void clearHover() noexcept { m_hovered.reset(); }
    [[nodiscard]] const std::optional<world::RoomId>& hovered() const noexcept { return m_hovered; }
    /// The hovered plaque's box and its system's name (the hover's label); none without one.
    [[nodiscard]] std::optional<std::pair<core::Aabb, std::string>> hoveredLabel() const;
    /// The card of the hovered plaque; none without one. `logoSource` gives a system's logo as an RML source ("" none).
    [[nodiscard]] std::optional<PlaqueCardContent>
    hoveredCard(const std::function<std::string(const std::string&)>& logoSource) const;

    [[nodiscard]] std::size_t placed() const noexcept { return m_plaques.size(); }
    [[nodiscard]] std::size_t drawn() const noexcept;
    [[nodiscard]] std::size_t failed() const noexcept { return m_failed; }

private:
    struct Shared;
    struct Plaque {
        PlaqueSite site;
        std::string system;
        assets::PlaqueText text;
        std::filesystem::path logo;
        std::optional<scene::MaterialHandle> handle;
        std::string materialKey;
        std::optional<gl::Texture2D> texture;
        bool pending = false;
        bool failed = false;
        bool applied = false;
    };

    void rescan(const Frame& frame);
    void takeOff(gl::MaterialRegistry& materials);
    void ask(Plaque& plaque);

    core::WorkerPool& m_pool;
    std::filesystem::path m_assetRoot;
    std::shared_ptr<Shared> m_shared;
    core::JobGeneration m_generation;
    bool m_dirty = true;
    std::optional<std::uint64_t> m_systemsRevision;
    /// By room id text.
    std::map<std::string, Plaque, std::less<>> m_plaques;
    std::optional<world::RoomId> m_hovered;
    std::size_t m_failed = 0;
};

} // namespace rv::app
