#pragma once

// The shelves' letter tabs, caption cards and section signs in the scene shown (I12 step 5, ADR-0021 §1;
// shelf_labels.hpp says what and where). For each room of the floor shown, its tabs are one object ("tabs:<room>", a
// label_plates prefab), its cards another ("captions:<room>"), and each section sign of a shared or arcade room one
// more ("section:<room>/<n>"), added to the scene at runtime through the host's replaceObjects() under their prefixes.
// The tabs' and the cards' atlases are drawn on the worker pool (assets::makeTabAtlas(), makeCaptionAtlas(): the
// bundled faces; BC1 where the GPU has S3TC), uploaded on the main thread, two a frame, under
// gl::GpuMemoryKind::LabelTextures, and put on the object's picture as a texture override, as the plaques' plates are.
// The section signs' pictures are WorldSigns' (their generic one names the section; its identity's sign goes over it).
//
// The labels follow the floor shown (WorldSession::shelfLabels() and its revision: a floor shown, a live update, the
// room editor's commit), found again when the scene changes too; a room whose objects changed is put in the scene one
// a frame (every one in a lockstep frame: a floor's plates built at once held the frame a ride arrives in for about
// 80 ms on the real library), and an atlas whose texts are the same is kept (a module moved keeps its tabs' atlas).
// While `frozen` (the room editor holds render items by index) nothing is added or taken away: the labels follow once
// the edit session ends.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/scene_instance.hpp>
#include <rv/app/shelf_labels.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/texture.hpp>

namespace rv::app {

class WorldShelfLabels {
public:
    struct Frame {
        scene::SceneDescription* description = nullptr;
        SceneInstance* instance = nullptr;
        gl::MaterialRegistry* materials = nullptr;
        /// The floor shown's rooms' labels, and their revision (a new one when they may have changed).
        std::vector<const ShelfLabels*> labels;
        std::uint64_t revision = 0;
        /// The atlases encoded as BC1 (the GPU has S3TC), else uploaded as RGBA8.
        bool compressed = false;
        /// A lockstep frame: the atlases asked are waited for (at most kSettleSeconds) and put on at once.
        bool settle = false;
        /// The room editor holds render items by index: no object added or taken away now.
        bool frozen = false;
        /// A ride is under way (its view waits for the covers in view, the screen black): no room placed now, outside a
        /// lockstep frame (the frames are the covers' uploads).
        bool hold = false;
        /// Replaces the scene's objects under a prefix (SceneInstance::replaceObjects() and the walker's collision);
        /// false when it could not.
        std::function<bool(const SceneObjectsChange&)> replaceObjects;
        /// The section signs were replaced: the identity's signs are to be found again (WorldSigns::sceneChanged()).
        std::function<void()> signsChanged;
    };
    static constexpr double kSettleSeconds = 5.0;
    /// Uploads a frame outside a lockstep one.
    static constexpr std::size_t kUploadsPerFrame = 2;

    WorldShelfLabels(core::WorkerPool& pool, std::filesystem::path assetRoot);
    ~WorldShelfLabels();
    WorldShelfLabels(const WorldShelfLabels&) = delete;
    WorldShelfLabels& operator=(const WorldShelfLabels&) = delete;

    void sceneChanged() noexcept { m_dirty = true; }
    /// Once a frame, with the scene shown.
    void update(const Frame& frame);
    /// The overrides taken off, the textures released, the jobs cancelled (the objects stay with their scene).
    void release(gl::MaterialRegistry* materials);

    /// The scene's labels as the last update() left them: the tabs, cards and section signs placed, the atlases wanted
    /// and those on their objects, the atlases that could not be drawn or uploaded during the run, their GPU bytes.
    [[nodiscard]] std::size_t tabs() const noexcept { return m_tabs; }
    [[nodiscard]] std::size_t captions() const noexcept { return m_captions; }
    [[nodiscard]] std::size_t sections() const noexcept { return m_sections; }
    [[nodiscard]] std::size_t atlases() const noexcept { return m_atlases.size(); }
    [[nodiscard]] std::size_t atlasesShown() const noexcept;
    [[nodiscard]] std::size_t failed() const noexcept { return m_failed; }
    [[nodiscard]] std::uint64_t bytes() const noexcept;

private:
    struct Shared;
    struct Atlas {
        std::string objectId;
        bool tabs = true;
        std::vector<std::string> tabTexts;
        std::vector<assets::CaptionText> captionTexts;
        bool compressed = false;
        std::optional<scene::MaterialHandle> handle;
        std::string materialKey;
        std::optional<gl::Texture2D> texture;
        std::uint64_t bytes = 0;
        bool pending = false;
        bool applied = false;

        [[nodiscard]] bool sameContent(const Atlas& other) const {
            return tabs == other.tabs && tabTexts == other.tabTexts && captionTexts == other.captionTexts &&
                   compressed == other.compressed;
        }
    };

    /// A room's objects as the scene should hold them (none: a room whose labels are to be taken away) and their
    /// atlases.
    struct RoomLabels {
        std::string room;
        std::vector<scene::SceneObject> tabs;
        std::vector<scene::SceneObject> captions;
        std::vector<scene::SceneObject> signs;
        std::vector<Atlas> atlases;
    };

    void rescan(const Frame& frame);
    /// The rooms queued since the last rescan put in the scene: one a frame (every one in a lockstep frame), none while
    /// frozen.
    void placeRooms(const Frame& frame);
    /// A room's atlases taken from those drawn before, else asked for.
    void adopt(const Frame& frame, RoomLabels& room);
    /// The objects under `prefix` made `wanted` unless the scene holds them already; false when they could not be.
    bool place(const Frame& frame, std::string_view prefix, std::vector<scene::SceneObject> wanted, bool& replaced);
    [[nodiscard]] static bool sameObjects(const scene::SceneDescription& description, std::string_view prefix,
                                          const std::vector<scene::SceneObject>& wanted);
    void takeOff(gl::MaterialRegistry& materials);
    void ask(Atlas& atlas);

    core::WorkerPool& m_pool;
    std::filesystem::path m_assetRoot;
    std::shared_ptr<Shared> m_shared;
    core::JobGeneration m_generation;
    bool m_dirty = true;
    std::optional<std::uint64_t> m_revision;
    /// By object id: those of the rooms placed, and those of before while rooms wait in the queue.
    std::map<std::string, Atlas, std::less<>> m_atlases;
    std::map<std::string, Atlas, std::less<>> m_previous;
    std::vector<RoomLabels> m_queue;
    std::size_t m_tabs = 0;
    std::size_t m_captions = 0;
    std::size_t m_sections = 0;
    std::size_t m_failed = 0;
};

} // namespace rv::app
