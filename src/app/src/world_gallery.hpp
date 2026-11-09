#pragma once

// The gallery's pictures in the scene shown (I12 step 2, ADR-0021 §2). The scene's frames (galleryTargets()) keep the
// pack's print in their scene file; for each frame whose room has a candidate of its kind (chooseGalleryPictures() over
// WorldSession::galleryOf()), the user's medium is read and fitted on the worker pool (makeGalleryPicture(): never
// stretched, on a mat of its own colour), its mip chain built and BC1-compressed when the GPU takes S3TC (else uploaded
// as RGBA8), uploaded on the main thread, two a frame, under gl::GpuMemoryKind::GalleryTextures, and put on the frame's
// picture material as a texture override (gl::MaterialRegistry::setTextureOverride). Equal pictures share a texture.
// The pictures of the whole floor shown stay resident: a floor is some tens of frames of about 0.4 MiB each, and the
// 4 GiB budget (ADR-0021 §8) holds them; a ride's floor finds its own and releases the others. A guard: the pictures
// asked for one scene stop at kCapBytes (their estimated size), the frames past it keep their prints (logged once). A
// medium that cannot be read or decoded is logged once (its name at debug level: it is the user's) and its frame keeps
// its print; it is not asked again in the run. Off (world.gallery false), every frame keeps its print.
//
// The frames are found again when the scene changes (sceneChanged(): a floor shown, objects replaced).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <rv/app/gallery_pictures.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/render/texture.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

class SceneInstance;

class WorldGallery {
public:
    /// What a frame gives: the scene shown, its materials, each room's gallery (the session's).
    struct Frame {
        const scene::SceneDescription* description = nullptr;
        SceneInstance* instance = nullptr;
        gl::MaterialRegistry* materials = nullptr;
        /// A room's gallery by its id ("room:snes"; "" for a scene of one room without prefixes); none: every frame
        /// keeps its print.
        std::function<RoomGallery(const std::string& room)> gallery;
        /// The GPU takes BC1 blocks (gl::CollectionArrayFormat::Bc1Srgb): the pictures go compressed.
        bool compressed = false;
        /// A lockstep run's frame (I3 step 12's rule): the pictures asked are waited for (at most kSettleSeconds) and
        /// all put on at once, so that its frames never depend on the pool's timing.
        bool settle = false;
    };
    static constexpr double kSettleSeconds = 10.0;
    /// The pictures of one scene at most (estimated: BC1 with mips, or RGBA8 without S3TC).
    static constexpr std::uint64_t kCapBytes = 256ull * 1024ull * 1024ull;

    explicit WorldGallery(core::WorkerPool& pool);
    ~WorldGallery();
    WorldGallery(const WorldGallery&) = delete;
    WorldGallery& operator=(const WorldGallery&) = delete;

    /// The scene was replaced or some of its objects were: its frames are found again on the next update().
    void sceneChanged() noexcept { m_dirty = true; }
    /// world.gallery (world.json, settings.json): off, every frame keeps its print; a change applies on the next
    /// update().
    void setEnabled(bool on) noexcept {
        if (on != m_enabled) {
            m_enabled = on;
            m_dirty = true;
        }
    }
    [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
    /// Once a frame, with the scene shown (none: nothing to do).
    void update(const Frame& frame);
    /// Every override taken off `materials` (when they still hold them), the textures released, the jobs cancelled.
    void release(gl::MaterialRegistry* materials);

    /// The frames of the scene shown, those with a medium chosen, those showing it now; the media that could not be
    /// shown during the run; the pictures' bytes on the GPU now and at most during the run.
    [[nodiscard]] std::size_t frames() const noexcept { return m_frames; }
    [[nodiscard]] std::size_t withMedia() const noexcept { return m_targets.size(); }
    [[nodiscard]] std::size_t shown() const noexcept;
    [[nodiscard]] std::size_t failed() const noexcept { return m_failed.size(); }
    /// The frames of the scene shown left with their prints by the guard.
    [[nodiscard]] std::size_t capped() const noexcept { return m_capped; }
    [[nodiscard]] std::uint64_t bytes() const noexcept;
    [[nodiscard]] std::uint64_t peakBytes() const noexcept { return m_peakBytes; }

private:
    struct Shared;
    /// A frame with a medium chosen: its picture's material and the picture it takes.
    struct Target {
        std::string objectId;
        scene::MaterialHandle handle;
        std::string materialKey;
        std::string request;
        bool applied = false;
    };
    /// A picture for one or more frames, by its request.
    struct Drawn {
        std::optional<gl::Texture2D> texture;
        /// Asked and not back yet: waiting in the queue, or on the pool once submitted.
        bool pending = false;
        bool submitted = false;
    };
    /// A picture waiting for a place on the pool.
    struct Ask {
        std::string request;
        GalleryChoice choice;
        bool compressed = false;
    };

    void rescan(const Frame& frame);
    /// Submits the queue's pictures while fewer than kMaxInFlight are on the pool (all with `settle`).
    void pump(bool settle);
    void takeOff(gl::MaterialRegistry& materials);

    core::WorkerPool& m_pool;
    std::shared_ptr<Shared> m_shared;
    core::JobGeneration m_generation;
    bool m_dirty = true;
    bool m_enabled = true;
    std::size_t m_frames = 0;
    std::size_t m_capped = 0;
    std::vector<Target> m_targets;
    std::map<std::string, Drawn, std::less<>> m_drawn;
    std::vector<Ask> m_queue;
    std::set<std::string, std::less<>> m_failed;
    std::uint64_t m_peakBytes = 0;
};

} // namespace rv::app
