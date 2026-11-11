#pragma once

// The rooms' sound at runtime (I12 step 7, ADR-0021 §5), on the clips' mixer: the room tone's loops of the scene's
// "sound" (room_sound_policy.hpp's emitters, one mixer loop each on the ambience bus, the eye's area's heard, the
// others faded out), the walker's steps by the floor under its feet and the interaction's clack, case and click as
// one-shots on the effects bus, and the menus' tick, which plays over the duck. Each sound is synthesized the first
// time it is needed (audio::synthesize, its seed fixed: a run sounds the same every time). Found again when the scene
// changes (sceneChanged()). Main thread only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/app/room_sound_policy.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/scene_instance.hpp>
#include <rv/audio/mixer.hpp>
#include <rv/audio/synth.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::app {

class ScreenService;

class RoomSound {
public:
    struct Frame {
        const scene::SceneDescription* description = nullptr;
        const SceneInstance* instance = nullptr;
        audio::Mixer* mixer = nullptr;
        /// The hero's picture and the clip heard (none: no picture, no clip).
        const ScreenService* screens = nullptr;
        glm::vec3 eye{0.0f};
        glm::vec3 forward{0.0f, 0.0f, -1.0f};
        /// The walker's feet (x, z).
        glm::vec2 feet{0.0f};
        /// The frame's simulated seconds, and the run's simulated time and frame (the report's events).
        double seconds = 0.0;
        double now = 0.0;
        std::uint64_t frame = 0;
        /// The walker walks by itself (free roam or the editor, no ride, no menu holding it).
        bool walking = false;
        /// In the headset: the steps lower.
        bool vr = false;
        /// The interaction's state this frame (none: no world).
        std::optional<WorldState> interaction;
        /// A menu's focus moved or a button was pressed this frame.
        bool uiTick = false;
    };

    /// The scene's objects changed: its emitters and rugs are found again at the next update().
    void sceneChanged() noexcept { m_dirty = true; }
    void update(const Frame& frame);

    /// The run report's figures (AudioRecord's room sound).
    void record(AudioRecord& record) const;

private:
    struct Loop {
        audio::SoundId sound = audio::SoundId::CrtHum;
        audio::SampleSourceId source = 0;
        /// The emitter it plays (none: free, fading out or silent).
        std::optional<std::size_t> emitter;
    };

    void rescan(const Frame& frame);
    /// The bank's buffer of `sound` (variant `variant` of a footstep), synthesized the first time.
    std::shared_ptr<const audio::PcmBuffer> buffer(audio::SoundId sound, std::uint32_t variant = 0);
    void trigger(const Frame& frame, audio::SoundId sound, const audio::OneShot& shot);

    bool m_dirty = true;
    std::vector<RoomToneEmitter> m_emitters;
    std::vector<FloorPatch> m_patches;
    std::vector<Loop> m_loops;
    std::array<std::shared_ptr<const audio::PcmBuffer>, audio::kSoundIds.size() * kFootstepVariants> m_bank{};
    FootstepCadence m_cadence;
    std::optional<WorldState> m_lastState;
    std::size_t m_audible = 0;
    std::size_t m_audibleMax = 0;
    std::uint64_t m_effects = 0;
    std::uint64_t m_footsteps = 0;
    std::uint64_t m_ticks = 0;
    std::vector<AudioEffectRecord> m_events;
};

} // namespace rv::app
