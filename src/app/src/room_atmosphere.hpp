#pragma once

// The rooms' air at runtime (I12 step 6, ADR-0021 §4), beside the dust and light cones the renderer draws from the
// scene (scene::deriveAtmosphere): the hero signs of the rooms whose atmosphere says neonFlicker dip now and then
// (their picture materials' flicker, gl::MaterialRegistry::setFlicker; steady under reduceMotion), and the room whose
// CRT is the hero screen has its "accent:crt_glow" light follow the picture it shows (crt_glow.hpp, crtBreathing):
// eased, written at most every kCrtGlowUpdateSeconds, the light back as described when the clip stops, the hero moves
// to another room or reduceMotion is on. Found again when the scene changes (sceneChanged()). Main thread only.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/crt_glow.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/render/material_registry.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

class ScreenService;

class RoomAtmosphere {
public:
    struct Frame {
        const scene::SceneDescription* description = nullptr;
        SceneInstance* instance = nullptr;
        gl::MaterialRegistry* materials = nullptr;
        /// The room's screens: the hero's picture and its object (none: no glow follows anything).
        const ScreenService* screens = nullptr;
        /// world.reduceMotion: the signs steady, the glow at once.
        bool reduceMotion = false;
        /// The frame's simulated seconds.
        float seconds = 0.0f;
        /// A lockstep frame (--fixed-dt): the glow follows the picture at once, so that a run's last frame does not
        /// hang on when the clip's frames reached the screen.
        bool settle = false;
    };

    /// The scene's objects or lights changed: the signs and glows are found again at the next update().
    void sceneChanged() noexcept { m_dirty = true; }
    void update(const Frame& frame);

    /// The signs flickering, the glows that may follow a picture, the glow's writes so far.
    [[nodiscard]] std::size_t flickering() const noexcept { return m_flickered.size(); }
    [[nodiscard]] std::size_t glows() const noexcept { return m_glows.size(); }
    [[nodiscard]] std::size_t glowWrites() const noexcept { return m_glowWrites; }

private:
    struct Glow {
        /// Its room's prefix ("room:snes/") and its light, by index and name.
        std::string prefix;
        std::size_t light = 0;
        std::string name;
        CrtGlow base;
        CrtGlow current;
        CrtGlow written;
    };

    void rescan(const Frame& frame);
    /// Writes `glow`'s current values into its light; true when they changed.
    static bool write(SceneInstance& instance, Glow& glow);

    bool m_dirty = true;
    std::optional<bool> m_reduceMotion;
    std::vector<scene::MaterialHandle> m_flickered;
    std::vector<Glow> m_glows;
    bool m_hadPicture = false;
    float m_sinceWrite = 0.0f;
    std::size_t m_glowWrites = 0;
};

} // namespace rv::app
