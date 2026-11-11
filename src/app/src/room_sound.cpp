#include "room_sound.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

#include <rv/app/audio_policy.hpp>
#include <rv/audio/spatial.hpp>

#include "screen_service.hpp"

namespace rv::app {
namespace {

/// The bank's seed: every run's sounds the same.
constexpr std::uint32_t kBankSeed = 0x52564131u;

/// The area index of the eye's area id in `instance` (none in a scene without areas).
std::optional<std::size_t> eyeAreaOf(const SceneInstance& instance) {
    const std::string_view id = instance.areaId();
    const std::span<const scene::SceneArea> areas = instance.areas();
    for (std::size_t index = 0; index < areas.size(); ++index) {
        if (areas[index].id == id) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace

std::shared_ptr<const audio::PcmBuffer> RoomSound::buffer(audio::SoundId sound, std::uint32_t variant) {
    const std::size_t index =
        static_cast<std::size_t>(sound) * kFootstepVariants + std::min(variant, kFootstepVariants - 1);
    if (!m_bank[index]) {
        m_bank[index] =
            std::make_shared<const audio::PcmBuffer>(audio::synthesize(sound, kBankSeed + variant, audio::kMixRate));
    }
    return m_bank[index];
}

void RoomSound::rescan(const Frame& frame) {
    m_dirty = false;
    m_emitters = roomToneEmitters(*frame.description);
    m_patches = floorPatches(*frame.description, frame.instance->objectBounds());
    // Every loop lets its emitter go (it fades out unless taken again), then each emitter takes a free loop of its
    // sound or a new one.
    for (Loop& loop : m_loops) {
        loop.emitter.reset();
    }
    for (std::size_t index = 0; index < m_emitters.size(); ++index) {
        const audio::SoundId sound = m_emitters[index].sound;
        auto free = std::find_if(m_loops.begin(), m_loops.end(),
                                 [sound](const Loop& loop) { return loop.sound == sound && !loop.emitter; });
        if (free == m_loops.end()) {
            m_loops.push_back(Loop{sound, frame.mixer->addSampleSource(buffer(sound), audio::Bus::Ambience), {}});
            free = std::prev(m_loops.end());
        }
        free->emitter = index;
    }
    spdlog::info("Room sound: {} emitter(s), {} rug(s) and runner(s), {} loop(s)", m_emitters.size(), m_patches.size(),
                 m_loops.size());
}

void RoomSound::trigger(const Frame& frame, audio::SoundId sound, const audio::OneShot& shot) {
    if (frame.mixer->trigger(buffer(sound, sound == audio::SoundId::FootstepParquet ||
                                                   sound == audio::SoundId::FootstepRug ||
                                                   sound == audio::SoundId::FootstepRunner
                                               ? static_cast<std::uint32_t>(m_footsteps % kFootstepVariants)
                                               : 0u),
                             shot)) {
        ++m_effects;
        if (m_events.size() < kMaxAudioTimelineEntries) {
            m_events.push_back(
                AudioEffectRecord{frame.frame, frame.now, std::string(audio::toString(sound)), shot.gain, shot.pan});
        }
    }
}

void RoomSound::update(const Frame& frame) {
    if (frame.description == nullptr || frame.instance == nullptr || frame.mixer == nullptr) {
        return;
    }
    if (m_dirty) {
        rescan(frame);
    }
    // The tone: each loop at its emitter's gain for the eye, the others fading out.
    const std::optional<std::size_t> eyeArea = eyeAreaOf(*frame.instance);
    const bool showing = frame.screens != nullptr && frame.screens->heroShowsPicture();
    const bool clipHeard = frame.screens != nullptr && frame.screens->clipHeard();
    m_audible = 0;
    for (const Loop& loop : m_loops) {
        audio::SourceControl control;
        control.playing = true;
        if (loop.emitter) {
            const RoomToneMix mix =
                roomToneMix(m_emitters[*loop.emitter], frame.eye, frame.forward, eyeArea, showing, clipHeard);
            control.gain = mix.gain;
            control.pan = mix.pan;
            m_audible += mix.gain > 0.0 ? 1u : 0u;
        }
        frame.mixer->setSampleControl(loop.source, control);
    }
    m_audibleMax = std::max(m_audibleMax, m_audible);

    // The steps.
    if (const std::optional<FootstepCadence::Step> step = m_cadence.advance(frame.feet, frame.seconds, frame.walking)) {
        const audio::SoundId sound = footstepSound(surfaceUnder(frame.feet, m_patches));
        const double gain = kFootstepLevel * (frame.vr ? audio::gainFromDecibels(kFootstepVrDecibels) : 1.0);
        trigger(frame, sound,
                audio::OneShot{gain, step->left ? -kFootstepPan : kFootstepPan, audio::Bus::Effects, true});
        ++m_footsteps;
    }
    // The interaction's edges.
    if (frame.interaction && m_lastState) {
        if (const std::optional<audio::SoundId> sound = interactionSound(*m_lastState, *frame.interaction)) {
            trigger(frame, *sound, audio::OneShot{kInteractionLevel, 0.0, audio::Bus::Effects, true});
        }
    }
    m_lastState = frame.interaction;
    // The menus' tick, heard over their duck.
    if (frame.uiTick) {
        trigger(frame, audio::SoundId::UiTick, audio::OneShot{kUiTickLevel, 0.0, audio::Bus::Effects, false});
        ++m_ticks;
    }
}

void RoomSound::record(AudioRecord& record) const {
    record.ambienceSources = m_audibleMax;
    record.ambienceEmitters = m_emitters.size();
    record.effectsTriggered = m_effects;
    record.footsteps = m_footsteps;
    record.uiTicks = m_ticks;
    record.effects = m_events;
}

} // namespace rv::app
