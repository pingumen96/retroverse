#pragma once

// The rooms' life at runtime (I12 step 8, ADR-0021 §6; the GPU-free rules: rv/app/room_life.hpp): the scene's moving
// parts turned each frame (a fan's blades, a clock's hands: SceneInstance::moveParts()), the media racks' activity
// LEDs blinking and the lava lamps' wax breathing (their materials' flicker, gl::MaterialRegistry::setFlicker: steady
// under reduceMotion). Found again when the scene changes (sceneChanged()). Main thread only.

#include <cstddef>
#include <optional>
#include <vector>

#include <rv/app/scene_instance.hpp>
#include <rv/render/material_registry.hpp>

namespace rv::app {

class RoomLife {
public:
    struct Frame {
        SceneInstance* instance = nullptr;
        gl::MaterialRegistry* materials = nullptr;
        /// world.reduceMotion: the fans still, the clocks' second hands at twelve, the LEDs and the wax steady.
        bool reduceMotion = false;
        /// The frame's simulated seconds, and the seconds into the day the clocks show.
        double seconds = 0.0;
        double clockSeconds = 0.0;
    };

    /// The scene's objects changed: the moving parts and the materials are found again at the next update().
    void sceneChanged() noexcept { m_dirty = true; }
    void update(const Frame& frame);

    /// The scene's moving parts by kind (the fans' blades, the clocks' hands), the materials blinking and breathing,
    /// and the frames that moved a part.
    [[nodiscard]] std::size_t spinning() const noexcept { return m_spinning; }
    [[nodiscard]] std::size_t clockHands() const noexcept { return m_clockHands; }
    [[nodiscard]] std::size_t blinking() const noexcept { return m_blinking; }
    [[nodiscard]] std::size_t breathing() const noexcept { return m_breathing; }
    [[nodiscard]] std::size_t movedFrames() const noexcept { return m_movedFrames; }

private:
    void rescan(const Frame& frame);

    bool m_dirty = true;
    std::optional<bool> m_reduceMotion;
    std::vector<scene::MaterialHandle> m_flickering;
    std::size_t m_spinning = 0;
    std::size_t m_clockHands = 0;
    std::size_t m_blinking = 0;
    std::size_t m_breathing = 0;
    std::size_t m_movedFrames = 0;
};

} // namespace rv::app
